#include "occlusionstorage.hpp"

#include <components/debug/debuglog.hpp>
#include <components/debug/frametrace.hpp>
#include <components/files/hash.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/vfs/manager.hpp>
#include <functional>
#include <sstream>

#include <sqlite3.h>

#include <cmath>
#include <cstring>
#include <vector>

namespace OcclusionCulling
{
    namespace
    {
        class PrepareMesh final : public SceneUtil::WorkItem
        {
        public:
            explicit PrepareMesh(std::function<void()> work)
                : mWork(std::move(work))
            {
            }
            void doWork() override { mWork(); }

        private:
            std::function<void()> mWork;
        };

        template <class T>
        bool readPod(const std::byte*& cursor, std::size_t& remaining, T& out)
        {
            if (remaining < sizeof(T))
                return false;
            std::memcpy(&out, cursor, sizeof(T));
            cursor += sizeof(T);
            remaining -= sizeof(T);
            return true;
        }
    }

    OcclusionStorage::OcclusionStorage(const std::string& databasePath, const VFS::Manager* vfs)
        : mVfs(vfs)
        , mBuildQueue(std::make_unique<SceneUtil::WorkQueue>(1))
    {
        if (databasePath.empty())
            return;

        const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(databasePath.c_str(), &mDb, flags, nullptr) != SQLITE_OK)
        {
            Log(Debug::Warning) << "Failed to open occlusion cache database \"" << databasePath << "\": "
                                << sqlite3_errmsg(mDb);
            if (mDb)
            {
                sqlite3_close(mDb);
                mDb = nullptr;
            }
            return;
        }

        constexpr const char* schema = R"(
            CREATE TABLE IF NOT EXISTS occlusion_meshes (
                model_path  TEXT    NOT NULL,
                mesh_res    INTEGER NOT NULL,
                shrink_key  INTEGER NOT NULL,
                mesh_blob   BLOB    NOT NULL,
                PRIMARY KEY (model_path, mesh_res, shrink_key)
            );
        )";
        char* errMsg = nullptr;
        if (sqlite3_exec(mDb, schema, nullptr, nullptr, &errMsg) != SQLITE_OK)
        {
            Log(Debug::Warning) << "Failed to create occlusion cache schema: " << errMsg;
            sqlite3_free(errMsg);
        }

        sqlite3_exec(mDb, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(mDb, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

        int rowCount = 0;
        sqlite3_stmt* countStmt = nullptr;
        if (sqlite3_prepare_v2(mDb, "SELECT COUNT(*) FROM occlusion_meshes", -1, &countStmt, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(countStmt) == SQLITE_ROW)
                rowCount = sqlite3_column_int(countStmt, 0);
            sqlite3_finalize(countStmt);
        }
        Log(Debug::Info) << "OcclusionStorage: opened cache at \"" << databasePath
                         << "\" — " << rowCount << " precomputed meshes";
    }

    OcclusionStorage::~OcclusionStorage()
    {
        // Stop job chaining before cancelling/joining the worker queue.
        {
            const std::lock_guard lock(mReadyMutex);
            mStopping = true;
            mDeferred.clear();
            mDeferredKeys.clear();
        }
        mBuildQueue.reset();
        if (mDb)
            sqlite3_close(mDb);
    }

    std::string OcclusionStorage::persistentModelKey(std::string_view modelPath) const
    {
        {
            const std::lock_guard lock(mMutex);
            if (const auto it = mContentKeys.find(std::string(modelPath)); it != mContentKeys.end())
                return it->second;
        }
        // VFS resolves the winning mod/archive entry. Hash its bytes rather than
        // just its name, so changed assets never reuse the old occluder geometry.
        std::ostringstream key;
        key << "mesh-v3:" << modelPath;
        if (mVfs)
        {
            try
            {
                auto stream = mVfs->get(VFS::Path::Normalized(modelPath));
                const auto hash = Files::getHash(modelPath, *stream);
                key << ':' << std::hex << hash[0] << ':' << hash[1];
            }
            catch (const std::exception&)
            {
                return {}; // An optional cache must not break model loading.
            }
        }
        const std::lock_guard lock(mMutex);
        return mContentKeys.emplace(std::string(modelPath), key.str()).first->second;
    }

    void OcclusionStorage::prepare(
        std::string modelPath, osg::ref_ptr<const osg::Node> source, int meshRes, float shrinkFactor)
    {
        if (!source || modelPath.empty())
            return;
        const int shrinkKey = makeShrinkKey(shrinkFactor);
        const std::string key = makeKey(modelPath, meshRes, shrinkKey);
        const std::unique_lock submissionLock(mReadyMutex);
        if (mStopping || mReady.contains(key) || mPending.contains(key) || mDeferredKeys.contains(key))
            return;
        if (mPending.size() >= 128)
        {
            // Keep only a weak reference in the overflow backlog. Loading a
            // dense cell must not permanently lose most preparation requests.
            if (mDeferred.size() < 4096)
            {
                mDeferred.push_back({ key, std::move(modelPath), source.get(), meshRes, shrinkFactor });
                mDeferredKeys.insert(key);
                ++mDeferredCount;
            }
            else
                ++mRejected;
            return;
        }
        mPending.insert(key);
        ++mQueued;
        // Submission stays under the ready mutex so shutdown cannot reset the
        // queue between checking mStopping and adding a chained job.
        mBuildQueue->addWorkItem(
            std::make_shared<PrepareMesh>([this, key, modelPath = std::move(modelPath), source = std::move(source),
                                              meshRes, shrinkFactor, shrinkKey] {
                Debug::Trace::Scope trace("Prepare immutable occluder mesh");
                std::shared_ptr<const OccluderMesh> ready;
                try
                {
                    auto mesh = std::make_shared<OccluderMesh>();
                    if (!get(modelPath, meshRes, shrinkKey, *mesh))
                    {
                        recordMiss();
                        *mesh = buildSimplifiedMesh(const_cast<osg::Node*>(source.get()), meshRes, shrinkFactor);
                        put(modelPath, meshRes, shrinkKey, *mesh);
                    }
                    ready = std::move(mesh);
                }
                catch (const std::exception& error)
                {
                    Log(Debug::Warning) << "Could not prepare occluder " << modelPath << ": " << error.what();
                }
                DeferredRequest next;
                osg::ref_ptr<const osg::Node> nextSource;
                {
                    const std::lock_guard lock(mReadyMutex);
                    mPending.erase(key);
                    mReady.emplace(key, std::move(ready));
                    while (!mStopping && !mDeferred.empty())
                    {
                        next = std::move(mDeferred.front());
                        mDeferred.pop_front();
                        mDeferredKeys.erase(next.key);
                        if (next.source.lock(nextSource))
                            break;
                    }
                }
                if (nextSource)
                    prepare(std::move(next.model), std::move(nextSource), next.resolution, next.shrink);
            }));
    }

    std::shared_ptr<const OccluderMesh> OcclusionStorage::getReady(
        std::string_view modelPath, int meshRes, int shrinkKey) const
    {
        const std::unique_lock lock(mReadyMutex, std::try_to_lock);
        if (!lock.owns_lock())
            return {};
        const auto it = mReady.find(makeKey(modelPath, meshRes, shrinkKey));
        if (it == mReady.end())
            return {};
        mReadyHits.fetch_add(1, std::memory_order_relaxed);
        return it->second;
    }

    bool OcclusionStorage::isOpen() const
    {
        return mDb != nullptr;
    }

    int OcclusionStorage::makeShrinkKey(float shrinkFactor)
    {
        return static_cast<int>(shrinkFactor * 1000000.0f + (shrinkFactor >= 0.f ? 0.5f : -0.5f));
    }

    std::string OcclusionStorage::makeKey(std::string_view modelPath, int meshRes, int shrinkKey) const
    {
        return std::string(modelPath) + "|" + std::to_string(meshRes) + "|" + std::to_string(shrinkKey);
    }

    bool OcclusionStorage::deserialize(const void* data, int bytes, OccluderMesh& out) const
    {
        const auto* cursor = reinterpret_cast<const std::byte*>(data);
        std::size_t remaining = static_cast<std::size_t>(bytes);

        float mins[3];
        float maxs[3];
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;

        if (!readPod(cursor, remaining, mins)
            || !readPod(cursor, remaining, maxs)
            || !readPod(cursor, remaining, vertexCount)
            || !readPod(cursor, remaining, indexCount))
            return false;

        // Validate counts before allocating, including on truncated/corrupt rows.
        if (vertexCount > remaining / sizeof(osg::Vec3f))
            return false;
        const std::size_t vertexBytes = sizeof(osg::Vec3f) * vertexCount;
        if (indexCount > (remaining - vertexBytes) / sizeof(unsigned int) || indexCount % 3 != 0)
            return false;
        const std::size_t indexBytes = sizeof(unsigned int) * indexCount;
        out = {};
        out.aabb.set(mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2]);
        out.vertices.resize(vertexCount);
        out.indices.resize(indexCount);

        if (vertexBytes)
        {
            std::memcpy(out.vertices.data(), cursor, vertexBytes);
            cursor += vertexBytes;
            remaining -= vertexBytes;
        }
        if (indexBytes)
            std::memcpy(out.indices.data(), cursor, indexBytes);
        if (!out.vertices.empty() && !out.aabb.valid())
            return false;
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(mins[axis]) || !std::isfinite(maxs[axis]))
                return false;
        for (const auto& vertex : out.vertices)
            for (int axis = 0; axis < 3; ++axis)
                if (!std::isfinite(vertex[axis]))
                    return false;
        for (const auto index : out.indices)
            if (index >= vertexCount)
                return false;
        return true;
    }

    bool OcclusionStorage::get(std::string_view modelPath, int meshRes, int shrinkKey, OccluderMesh& out) const
    {
        if (!mDb)
            return false;

        const std::string persistentPath = persistentModelKey(modelPath);
        if (persistentPath.empty())
            return false;
        const std::string key = makeKey(persistentPath, meshRes, shrinkKey);
        {
            const std::lock_guard lock(mMutex);
            const auto it = mCache.find(key);
            if (it != mCache.end())
            {
                out = *it->second;
                ++mMemHits;
                return true;
            }
        }

        Debug::Trace::Scope trace("Occluder database read");
        const std::lock_guard dbLock(mDbMutex);
        sqlite3_stmt* stmt = nullptr;
        constexpr const char* sql = "SELECT mesh_blob FROM occlusion_meshes WHERE model_path = ?1 AND mesh_res = ?2 AND shrink_key = ?3";
        if (sqlite3_prepare_v2(mDb, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return false;

        sqlite3_bind_text(stmt, 1, persistentPath.data(), static_cast<int>(persistentPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, meshRes);
        sqlite3_bind_int(stmt, 3, shrinkKey);

        bool found = false;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            const void* blob = sqlite3_column_blob(stmt, 0);
            const int blobSize = sqlite3_column_bytes(stmt, 0);
            found = blob != nullptr && blobSize > 0 && deserialize(blob, blobSize, out);
        }
        sqlite3_finalize(stmt);

        if (found)
        {
            ++mDbHits;
            const auto cached = std::make_shared<OccluderMesh>(out);
            const std::lock_guard lock(mMutex);
            mCache.emplace(key, cached);
        }

        return found;
    }

    OcclusionStorage::Stats OcclusionStorage::getAndResetStats()
    {
        Stats s;
        s.queued = mQueued.exchange(0);
        s.deferred = mDeferredCount.exchange(0);
        s.rejected = mRejected.exchange(0);
        s.readyHits = mReadyHits.exchange(0);
        s.memHits = mMemHits.exchange(0);
        s.dbHits  = mDbHits.exchange(0);
        s.misses  = mMisses.exchange(0);
        s.writes  = mWrites.exchange(0);
        return s;
    }

    std::vector<std::byte> OcclusionStorage::serialize(const OccluderMesh& mesh) const
    {
        const float mins[3] = { mesh.aabb.xMin(), mesh.aabb.yMin(), mesh.aabb.zMin() };
        const float maxs[3] = { mesh.aabb.xMax(), mesh.aabb.yMax(), mesh.aabb.zMax() };
        const std::uint32_t vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        const std::uint32_t indexCount  = static_cast<std::uint32_t>(mesh.indices.size());

        const std::size_t totalBytes = sizeof(mins) + sizeof(maxs)
            + sizeof(vertexCount) + sizeof(indexCount)
            + sizeof(osg::Vec3f) * mesh.vertices.size()
            + sizeof(unsigned int) * mesh.indices.size();

        std::vector<std::byte> buf(totalBytes);
        std::byte* p = buf.data();

        auto write = [&](const void* src, std::size_t n) {
            std::memcpy(p, src, n);
            p += n;
        };

        write(mins, sizeof(mins));
        write(maxs, sizeof(maxs));
        write(&vertexCount, sizeof(vertexCount));
        write(&indexCount, sizeof(indexCount));
        if (!mesh.vertices.empty())
            write(mesh.vertices.data(), sizeof(osg::Vec3f) * mesh.vertices.size());
        if (!mesh.indices.empty())
            write(mesh.indices.data(), sizeof(unsigned int) * mesh.indices.size());

        return buf;
    }

    void OcclusionStorage::put(std::string_view modelPath, int meshRes, int shrinkKey, const OccluderMesh& mesh)
    {
        if (!mDb)
            return;

        const std::string persistentPath = persistentModelKey(modelPath);
        if (persistentPath.empty())
            return;
        const std::string key = makeKey(persistentPath, meshRes, shrinkKey);
        Debug::Trace::Scope trace("Occluder database write");
        const std::lock_guard dbLock(mDbMutex);
        const std::vector<std::byte> blob = serialize(mesh);
        if (blob.empty())
            return;

        constexpr const char* sql = R"(
            INSERT OR REPLACE INTO occlusion_meshes (model_path, mesh_res, shrink_key, mesh_blob)
            VALUES (?1, ?2, ?3, ?4)
        )";

        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(mDb, sql, -1, &stmt, nullptr) != SQLITE_OK)
            return;

        sqlite3_bind_text(stmt, 1, persistentPath.data(), static_cast<int>(persistentPath.size()), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, meshRes);
        sqlite3_bind_int(stmt, 3, shrinkKey);
        sqlite3_bind_blob(stmt, 4, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        ++mWrites;

        const auto cached = std::make_shared<OccluderMesh>(mesh);
        const std::lock_guard lock(mMutex);
        mCache.emplace(key, cached);
    }
}

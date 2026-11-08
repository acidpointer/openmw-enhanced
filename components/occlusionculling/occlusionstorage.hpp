#ifndef OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H
#define OPENMW_COMPONENTS_OCCLUSIONCULLING_OCCLUSIONSTORAGE_H

#include "occludermesh.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <osg/Node>
#include <osg/observer_ptr>
#include <osg/ref_ptr>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace VFS
{
    class Manager;
}
namespace SceneUtil
{
    class WorkQueue;
}

namespace OcclusionCulling
{
    /// SQLite-backed cache of precomputed occluder meshes.
    /// Keyed by (model_path, mesh_res, shrink_key).
    /// Thread-safe for concurrent get() calls; put() is also guarded by a mutex.
    class OcclusionStorage
    {
    public:
        explicit OcclusionStorage(const std::string& databasePath, const VFS::Manager* vfs = nullptr);
        ~OcclusionStorage();

        OcclusionStorage(const OcclusionStorage&) = delete;
        OcclusionStorage& operator=(const OcclusionStorage&) = delete;

        bool isOpen() const;

        // Called while loading an immutable resource template, never during cull.
        // Requests are deduplicated and the pending queue is bounded.
        void prepare(std::string modelPath, osg::ref_ptr<const osg::Node> source, int meshRes, float shrinkFactor);
        std::shared_ptr<const OccluderMesh> getReady(std::string_view modelPath, int meshRes, int shrinkKey) const;

        bool get(std::string_view modelPath, int meshRes, int shrinkKey, OccluderMesh& out) const;
        void put(std::string_view modelPath, int meshRes, int shrinkKey, const OccluderMesh& mesh);

        struct Stats
        {
            int queued = 0;
            int deferred = 0;
            int rejected = 0;
            int readyHits = 0;
            int memHits = 0; // served from in-memory map
            int dbHits  = 0; // loaded from SQLite
            int misses  = 0; // not in cache — buildSimplifiedMesh was called
            int writes  = 0; // written to SQLite
        };
        /// Returns accumulated stats since last call and resets counters.
        Stats getAndResetStats();

        void recordMiss() { ++mMisses; }

        static int makeShrinkKey(float shrinkFactor);

    private:
        bool deserialize(const void* data, int bytes, OccluderMesh& out) const;
        std::vector<std::byte> serialize(const OccluderMesh& mesh) const;
        std::string makeKey(std::string_view modelPath, int meshRes, int shrinkKey) const;

        std::string persistentModelKey(std::string_view modelPath) const;
        sqlite3* mDb = nullptr;
        const VFS::Manager* mVfs = nullptr;
        std::unique_ptr<SceneUtil::WorkQueue> mBuildQueue;
        mutable std::mutex mDbMutex;
        mutable std::mutex mReadyMutex;
        struct DeferredRequest
        {
            std::string key;
            std::string model;
            osg::observer_ptr<const osg::Node> source;
            int resolution;
            float shrink;
        };
        bool mStopping = false;
        std::deque<DeferredRequest> mDeferred;
        std::unordered_set<std::string> mDeferredKeys;
        std::unordered_set<std::string> mPending;
        std::unordered_map<std::string, std::shared_ptr<const OccluderMesh>> mReady;
        mutable std::unordered_map<std::string, std::string> mContentKeys;

        mutable std::mutex mMutex;
        mutable std::unordered_map<std::string, std::shared_ptr<OccluderMesh>> mCache;

        mutable std::atomic<int> mMemHits{ 0 };
        mutable std::atomic<int> mDbHits{ 0 };
        mutable std::atomic<int> mMisses{ 0 };
        mutable std::atomic<int> mWrites{ 0 };
        std::atomic<int> mQueued{ 0 };
        std::atomic<int> mDeferredCount{ 0 };
        std::atomic<int> mRejected{ 0 };
        mutable std::atomic<int> mReadyHits{ 0 };
    };
}

#endif

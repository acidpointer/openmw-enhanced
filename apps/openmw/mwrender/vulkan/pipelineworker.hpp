#ifndef OPENMW_MWRENDER_VULKAN_PIPELINEWORKER_H
#define OPENMW_MWRENDER_VULKAN_PIPELINEWORKER_H

#include "check.hpp"

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace MWRender::Vulkan
{
    struct CompiledEffectPipeline
    {
        VkDevice mDevice;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        explicit CompiledEffectPipeline(VkDevice device)
            : mDevice(device)
        {
        }
        ~CompiledEffectPipeline() { vkDestroyPipeline(mDevice, mPipeline, nullptr); }
    };

    // One worker owns its cache. The render thread never takes the compiler's
    // lock or waits on a future. Destroy before destroying render passes/device.
    class PipelineWorker
    {
    public:
        using Value = std::shared_ptr<CompiledEffectPipeline>;
        explicit PipelineWorker(VkDevice device, VkPipelineCache seed)
            : mDevice(device)
        {
            auto info = vkInfo<VkPipelineCacheCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
            std::vector<std::byte> bytes;
            std::size_t size = 0;
            if (seed && vkGetPipelineCacheData(device, seed, &size, nullptr) == VK_SUCCESS && size <= 64 * 1024 * 1024)
            {
                bytes.resize(size);
                if (size && vkGetPipelineCacheData(device, seed, &size, bytes.data()) == VK_SUCCESS)
                {
                    info.initialDataSize = size;
                    info.pInitialData = bytes.data();
                }
            }
            auto result = vkCreatePipelineCache(device, &info, nullptr, &mCache);
            if (result != VK_SUCCESS && info.initialDataSize)
            {
                info.initialDataSize = 0;
                info.pInitialData = nullptr;
                result = vkCreatePipelineCache(device, &info, nullptr, &mCache);
            }
            check(result, "Create effect worker pipeline cache");
            try
            {
                mThread = std::jthread([this](std::stop_token stop) {
                    for (;;)
                    {
                        Task task;
                        {
                            std::unique_lock lock(mMutex);
                            if (!mReady.wait(lock, stop, [&] { return !mQueue.empty(); }))
                                return;
                            if (stop.stop_requested())
                                return;
                            task = std::move(mQueue.front());
                            mQueue.pop_front();
                        }
                        task(mCache);
                    }
                });
            }
            catch (...)
            {
                vkDestroyPipelineCache(device, mCache, nullptr);
                throw;
            }
        }
        ~PipelineWorker()
        {
            stop();
            vkDestroyPipelineCache(mDevice, mCache, nullptr);
        }
        template <class Compile>
        std::future<Value> submit(Compile&& compile)
        {
            Task task(std::forward<Compile>(compile));
            auto result = task.get_future();
            {
                const std::lock_guard lock(mMutex);
                mQueue.push_back(std::move(task));
            }
            mReady.notify_one();
            return result;
        }
        // Draining is reserved for swapchain format changes and shutdown.
        void finish()
        {
            auto barrier = submit([](VkPipelineCache) { return Value{}; });
            barrier.get();
        }
        void stopAndMerge(VkPipelineCache destination)
        {
            stop();
            check(vkMergePipelineCaches(mDevice, destination, 1, &mCache), "Merge effect pipeline cache");
        }

    private:
        using Task = std::packaged_task<Value(VkPipelineCache)>;
        void stop()
        {
            mThread.request_stop();
            mReady.notify_all();
            if (mThread.joinable())
                mThread.join();
            mQueue.clear();
        }
        VkDevice mDevice;
        VkPipelineCache mCache = VK_NULL_HANDLE;
        std::mutex mMutex;
        std::condition_variable_any mReady;
        std::deque<Task> mQueue;
        std::jthread mThread;
    };
}
#endif

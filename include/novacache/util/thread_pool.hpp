#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace novacache::util {

class ThreadPool {
  public:
    explicit ThreadPool(std::size_t thread_count, std::size_t max_queue_size);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] bool try_submit(std::function<void()> job);
    void shutdown() noexcept;

    [[nodiscard]] std::size_t thread_count() const noexcept;
    [[nodiscard]] std::size_t queued_jobs() const;
    [[nodiscard]] bool accepting() const noexcept;

  private:
    void worker_main();

    std::size_t max_queue_size_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

} // namespace novacache::util

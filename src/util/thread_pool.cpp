#include "novacache/util/thread_pool.hpp"

#include <stdexcept>
#include <utility>

namespace novacache::util {

ThreadPool::ThreadPool(const std::size_t thread_count, const std::size_t max_queue_size)
    : max_queue_size_(max_queue_size) {
    if (thread_count == 0U) {
        throw std::invalid_argument("thread pool requires at least one worker");
    }
    if (max_queue_size == 0U) {
        throw std::invalid_argument("thread pool queue limit must be positive");
    }
    workers_.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        workers_.emplace_back([this] { worker_main(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

bool ThreadPool::try_submit(std::function<void()> job) {
    {
        const std::lock_guard lock{mutex_};
        if (stopping_ || jobs_.size() >= max_queue_size_) {
            return false;
        }
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() noexcept {
    {
        const std::lock_guard lock{mutex_};
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
    const std::lock_guard lock{mutex_};
    std::queue<std::function<void()>> empty;
    jobs_.swap(empty);
}

std::size_t ThreadPool::thread_count() const noexcept { return workers_.size(); }

std::size_t ThreadPool::queued_jobs() const {
    const std::lock_guard lock{mutex_};
    return jobs_.size();
}

bool ThreadPool::accepting() const noexcept {
    const std::lock_guard lock{mutex_};
    return !stopping_;
}

void ThreadPool::worker_main() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_ && jobs_.empty()) {
                return;
            }
            job = std::move(jobs_.front());
            jobs_.pop();
        }
        job();
    }
}

} // namespace novacache::util

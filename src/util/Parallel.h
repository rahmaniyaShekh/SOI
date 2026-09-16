#pragma once
//
// A persistent worker pool for the colour-conversion hot path.
//
// Spawning threads per frame costs ~60-100us on Windows, which at 30fps is a
// meaningful slice of a 2ms conversion budget. The pool is created once and the
// calling thread participates in the work, so an N-thread pool uses N+1 cores.
//
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace soi {

class ThreadPool {
public:
    // workers == 0 picks (hardware_concurrency - 1), clamped to [0, 7], because
    // the caller thread also runs a chunk.
    explicit ThreadPool(unsigned workers = 0) {
        if (workers == 0) {
            const unsigned hc = std::thread::hardware_concurrency();
            workers = hc > 1 ? hc - 1 : 0;
            if (workers > 7) workers = 7;
        }
        threads_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
            threads_.emplace_back([this] { workerLoop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_)
            if (t.joinable()) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    size_t size() const { return threads_.size() + 1; }

    // Splits [0, count) into size() contiguous chunks and runs fn(begin, end) on
    // each. Blocks until every chunk has finished.
    void parallelFor(size_t count, const std::function<void(size_t, size_t)>& fn) {
        if (count == 0) return;

        const size_t chunks = std::min(size(), count);
        if (chunks <= 1 || threads_.empty()) { fn(0, count); return; }

        const size_t per = (count + chunks - 1) / chunks;

        {
            std::lock_guard lk(m_);
            job_       = &fn;
            jobCount_  = count;
            jobPer_    = per;
            // Atomic: a lingering worker may read this outside the lock on its
            // way out of drainChunks().
            jobChunks_.store(chunks, std::memory_order_release);

            // Ordering matters. A worker from the *previous* job can still be
            // spinning in drainChunks(); it escapes only once it reads an index
            // past jobChunks_. Until nextChunk_ is republished it cannot claim
            // anything, so remaining_ must be armed first -- otherwise such a
            // worker could claim a chunk of this job and decrement a counter
            // that is still zero, underflowing it and hanging the wait below.
            remaining_.store(chunks, std::memory_order_release);
            nextChunk_.store(1, std::memory_order_release);   // chunk 0 is ours
            ++generation_;
        }
        cv_.notify_all();

        runChunk(0);          // caller does its share
        drainChunks();        // then helps with any chunk no worker picked up

        std::unique_lock lk(doneM_);
        doneCv_.wait(lk, [this] {
            return remaining_.load(std::memory_order_acquire) == 0;
        });
    }

private:
    void runChunk(size_t index) {
        const size_t begin = index * jobPer_;
        if (begin >= jobCount_) { finishOne(); return; }
        const size_t end = std::min(begin + jobPer_, jobCount_);
        (*job_)(begin, end);
        finishOne();
    }

    void drainChunks() {
        for (;;) {
            const size_t idx = nextChunk_.fetch_add(1, std::memory_order_acq_rel);
            if (idx >= jobChunks_.load(std::memory_order_acquire)) break;
            runChunk(idx);
        }
    }

    void finishOne() {
        if (remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard lk(doneM_);
            doneCv_.notify_all();
        }
    }

    void workerLoop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock lk(m_);
                cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            drainChunks();
        }
    }

    std::vector<std::thread> threads_;

    std::mutex              m_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    uint64_t                generation_ = 0;

    const std::function<void(size_t, size_t)>* job_ = nullptr;
    size_t                                     jobCount_ = 0;
    size_t                                     jobPer_ = 0;
    std::atomic<size_t>                        jobChunks_{0};
    std::atomic<size_t>                        nextChunk_{0};
    std::atomic<size_t>                        remaining_{0};

    std::mutex              doneM_;
    std::condition_variable doneCv_;
};

// Process-wide pool shared by the conversion path.
inline ThreadPool& sharedPool() {
    static ThreadPool pool;
    return pool;
}

} // namespace soi

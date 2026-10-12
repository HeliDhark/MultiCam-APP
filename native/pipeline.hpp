#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace multi {
// Decodes compressed frames on several threads and publishes them in capture order.
// One thread cannot decode a detailed 1080p MJPEG frame in 16.7 ms on older CPUs; two
// threads working on consecutive frames keep 60 FPS.
// submit() never blocks the capture thread: when every thread is busy and a frame is already
// waiting, the waiting frame is replaced by the newer one and counted in dropped().
class DecodePipeline {
public:
    // decode: runs on thread `thread` (0..threads-1); returns the decoded frame, valid until
    // that thread decodes again, or nullptr to skip the frame. publish: called in capture order.
    using Decode = std::function<const uint8_t*(int thread, const uint8_t* data, size_t bytes)>;
    using Publish = std::function<void(const uint8_t* frame)>;

    DecodePipeline(int threads, Decode decode, Publish publish) : decode_(std::move(decode)), publish_(std::move(publish)) {
        for (int i = 0; i < threads; ++i) threads_.emplace_back([this, i] { run(i); });
    }
    ~DecodePipeline() { finish(); }
    DecodePipeline(const DecodePipeline&) = delete;
    DecodePipeline& operator=(const DecodePipeline&) = delete;

    void submit(const uint8_t* data, size_t bytes) {
        std::vector<uint8_t> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            if (!spare_.empty()) { buffer.swap(spare_.back()); spare_.pop_back(); }
        }
        buffer.assign(data, data + bytes);  // copied outside the lock
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (waiting_) { ++dropped_; spare_.push_back(std::move(waitingData_)); }
            waitingData_.swap(buffer);
            waiting_ = true;
        }
        work_.notify_one();
    }

    uint64_t dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

    // Decodes and publishes what was already submitted, then stops the threads.
    void finish() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        work_.notify_all();
        for (auto& t : threads_) if (t.joinable()) t.join();
        threads_.clear();
    }

private:
    void run(int index) {
        std::vector<uint8_t> data;
        for (;;) {
            uint64_t ticket = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait(lock, [&] { return waiting_ || stopping_; });
                if (!waiting_) return;  // stopping, nothing left
                data.swap(waitingData_);
                waiting_ = false;
                ticket = nextTicket_++;
            }
            const uint8_t* frame = decode_(index, data.data(), data.size());
            {
                std::unique_lock<std::mutex> lock(mutex_);
                turn_.wait(lock, [&] { return nextOut_ == ticket; });
            }
            // Only the thread holding the turn publishes, so this runs outside the lock.
            if (frame) publish_(frame);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++nextOut_;
                if (spare_.size() < 4) spare_.push_back(std::move(data));
                data = std::vector<uint8_t>();
            }
            turn_.notify_all();
        }
    }

    Decode decode_;
    Publish publish_;
    std::mutex mutex_;
    std::condition_variable work_, turn_;
    std::vector<std::thread> threads_;
    std::vector<uint8_t> waitingData_;
    std::vector<std::vector<uint8_t>> spare_;  // reused buffers: no allocation per frame
    bool waiting_ = false, stopping_ = false;
    uint64_t nextTicket_ = 0, nextOut_ = 0, dropped_ = 0;
};
}

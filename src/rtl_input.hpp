// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <stdexcept>
#include <thread>

namespace ovmesh {

// RTL2832U transfers unsigned offset-binary I,Q. Center between 127 and 128:
// subtracting 128 would introduce an avoidable DC term into every sample.
inline std::complex<float> rtl_iq_sample(uint8_t i, uint8_t q) noexcept {
    return {(static_cast<float>(i) - 127.5f) / 128.0f,
            (static_cast<float>(q) - 127.5f) / 128.0f};
}

inline int nearest_rtl_gain(std::span<const int> supported, int requested) {
    if (supported.empty()) throw std::runtime_error("RTL-SDR tuner did not report any supported manual gains");
    return *std::min_element(supported.begin(), supported.end(), [=](int a, int b) {
        const auto da = std::abs(static_cast<int64_t>(a) - requested);
        const auto db = std::abs(static_cast<int64_t>(b) - requested);
        return da == db ? a < b : da < db;
    });
}

// librtlsdr's blocking async read initializes its cancellation state inside
// read_async. A single cancel immediately after launching a thread can arrive
// too early and be lost. Retry cancellation until that thread has returned;
// callers must keep its device open until stop() joins it. No detach/close race.
class RtlAsyncPump {
public:
    RtlAsyncPump() = default;
    ~RtlAsyncPump() { stop(); }
    RtlAsyncPump(const RtlAsyncPump&) = delete;
    RtlAsyncPump& operator=(const RtlAsyncPump&) = delete;

    void start(std::function<void()> read, std::function<void()> cancel) {
        stop();
        cancel_ = std::move(cancel);
        finished_ = false;
        try {
            thread_ = std::thread([this, read = std::move(read)] {
                // The receiver's read wrapper catches and publishes failures.
                try { read(); } catch (...) { }
                finished_ = true;
                finished_condition_.notify_all();
            });
        } catch (...) { finished_ = true; cancel_ = {}; throw; }
    }

    void stop() noexcept {
        if (!thread_.joinable()) return;
        while (!finished_.load()) {
            if (cancel_) { try { cancel_(); } catch (...) { } }
            std::unique_lock lock(mutex_);
            finished_condition_.wait_for(lock, std::chrono::milliseconds(10),
                [this] { return finished_.load(); });
        }
        thread_.join();
        cancel_ = {};
    }

private:
    std::thread thread_;
    std::atomic<bool> finished_{true};
    std::function<void()> cancel_;
    std::mutex mutex_;
    std::condition_variable finished_condition_;
};
} // namespace ovmesh

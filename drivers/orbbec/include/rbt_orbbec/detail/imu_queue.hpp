#pragma once

#include <rbt_orbbec/driver.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>

namespace rbt_orbbec::detail {

struct ImuSample {
    Kind kind { Kind::Accel };
    std::uint64_t index { 0 };
    std::uint64_t capture_us { 0 };
    std::array<float, 3> xyz {};
};

class ImuQueue {
  public:
    static constexpr std::size_t kCapacity = 256;
    static constexpr std::size_t kMaximumBatchSize = 64;

    struct Batch {
        std::array<ImuSample, kMaximumBatchSize> samples {};
        std::size_t size { 0 };
    };

    void push(const ImuSample& sample) {
        validate(sample);

        std::unique_lock lock(mutex_);
        if (closed_ || error_) {
            return;
        }
        if (size_ == ring_.size()) {
            error_ = std::make_exception_ptr(std::overflow_error("Orbbec IMU callback queue overflow"));
            const auto overflow = error_;
            lock.unlock();
            ready_.notify_all();
            std::rethrow_exception(overflow);
        }
        ring_[tail_] = sample;
        tail_ = (tail_ + 1U) % ring_.size();
        ++size_;
        lock.unlock();
        ready_.notify_one();
    }

    void fail(std::exception_ptr error) {
        if (!error) {
            return;
        }
        {
            std::lock_guard lock(mutex_);
            if (!error_) {
                error_ = std::move(error);
            }
        }
        ready_.notify_all();
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        ready_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] Batch wait_and_drain(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const auto has_work = [this] { return size_ != 0 || error_ || closed_; };
        if (!has_work() && timeout.count() > 0) {
            ready_.wait_for(lock, timeout, has_work);
        }
        if (error_) {
            std::rethrow_exception(error_);
        }

        Batch batch;
        batch.size = std::min(size_, batch.samples.size());
        for (std::size_t index = 0; index < batch.size; ++index) {
            batch.samples[index] = ring_[head_];
            head_ = (head_ + 1U) % ring_.size();
        }
        size_ -= batch.size;
        return batch;
    }

  private:
    static void validate(const ImuSample& sample) {
        if (sample.kind != Kind::Accel && sample.kind != Kind::Gyro) {
            throw std::runtime_error("Orbbec IMU callback produced a malformed sample kind");
        }
        if (!std::all_of(sample.xyz.begin(), sample.xyz.end(), [](float value) { return std::isfinite(value); })) {
            throw std::runtime_error("Orbbec IMU callback produced a non-finite sample");
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::array<ImuSample, kCapacity> ring_ {};
    std::size_t head_ { 0 };
    std::size_t tail_ { 0 };
    std::size_t size_ { 0 };
    std::exception_ptr error_;
    bool closed_ { false };
};

} // namespace rbt_orbbec::detail

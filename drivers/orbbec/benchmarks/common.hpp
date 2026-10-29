#pragma once

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench {

inline std::int64_t now_ns() {
    timespec value {};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        throw std::runtime_error("clock_gettime(CLOCK_MONOTONIC) failed");
    }
    constexpr auto billion = std::int64_t { 1000000000 };
    const auto seconds = static_cast<std::int64_t>(value.tv_sec);
    if (seconds < 0 || seconds > std::numeric_limits<std::int64_t>::max() / billion
        || (seconds == std::numeric_limits<std::int64_t>::max() / billion
            && value.tv_nsec > std::numeric_limits<std::int64_t>::max() % billion)) {
        throw std::overflow_error("CLOCK_MONOTONIC timestamp overflows nanoseconds");
    }
    return seconds * billion + value.tv_nsec;
}

class Stats {
  public:
    void mark_ready() {
        std::lock_guard lock(mutex_);
        ready_before_window_ = true;
    }

    void add(std::int64_t receive_ns,
             std::int64_t packaged_ns,
             std::uint64_t sdk_index,
             const std::uint8_t* data,
             std::size_t size) {
        if (receive_ns < 0 || packaged_ns < 0 || receive_ns < packaged_ns) {
            throw std::runtime_error("negative receive latency");
        }
        if (size != 0U && data == nullptr) {
            throw std::invalid_argument("non-empty frame data is null");
        }

        const auto latency = static_cast<std::uint64_t>(receive_ns - packaged_ns);
        std::lock_guard lock(mutex_);
        if (count_ == std::numeric_limits<std::uint64_t>::max()
            || size > std::numeric_limits<std::uint64_t>::max() - bytes_) {
            throw std::overflow_error("benchmark sample counters overflow");
        }
        if (!has_sample_) {
            has_sample_ = true;
            first_index_ = sdk_index;
            first_receive_ns_ = receive_ns;
        } else if (sdk_index == previous_index_) {
            increment(duplicates_, "duplicate counter overflows");
        } else if (sdk_index < previous_index_) {
            increment(reordered_, "reordered counter overflows");
        } else if (sdk_index - previous_index_ > 1U) {
            const auto missing = sdk_index - previous_index_ - 1U;
            if (missing > std::numeric_limits<std::uint64_t>::max() - gaps_) {
                throw std::overflow_error("SDK frame gap counter overflows");
            }
            gaps_ += missing;
        }

        previous_index_ = sdk_index;
        last_index_ = sdk_index;
        last_receive_ns_ = receive_ns;
        bytes_ += static_cast<std::uint64_t>(size);
        ++count_;
        max_latency_ns_ = std::max(max_latency_ns_, latency);
        update_checksum(data, size);
        add_latency_sample(latency);
    }

    [[nodiscard]] std::uint64_t count() const {
        std::lock_guard lock(mutex_);
        return count_;
    }

    [[nodiscard]] std::string json(std::int64_t start_ns, std::int64_t end_ns) const {
        if (end_ns <= start_ns) {
            throw std::invalid_argument("measurement end must be after start");
        }

        std::uint64_t count = 0U, bytes = 0U, gaps = 0U, duplicates = 0U, reordered = 0U;
        std::uint64_t first_index = 0U, last_index = 0U, max_latency_ns = 0U, checksum = kFnvOffset;
        std::int64_t first_receive_ns = 0, last_receive_ns = 0;
        bool has_sample = false, ready_before_window = false;
        std::vector<std::uint64_t> latencies;
        {
            std::lock_guard lock(mutex_);
            count = count_;
            bytes = bytes_;
            gaps = gaps_;
            duplicates = duplicates_;
            reordered = reordered_;
            first_index = first_index_;
            last_index = last_index_;
            first_receive_ns = first_receive_ns_;
            last_receive_ns = last_receive_ns_;
            max_latency_ns = max_latency_ns_;
            checksum = checksum_;
            has_sample = has_sample_;
            ready_before_window = ready_before_window_;
            latencies = latencies_;
        }

        std::sort(latencies.begin(), latencies.end());
        const auto interval_ns = end_ns - start_ns;
        const auto interval_seconds = static_cast<double>(interval_ns) / 1.0e9;
        const auto fps = static_cast<double>(count) / interval_seconds;
        const auto mib_per_second = static_cast<double>(bytes) / (1024.0 * 1024.0 * interval_seconds);

        std::ostringstream output;
        output << std::fixed << std::setprecision(9)
               << "{\"count\":" << count
               << ",\"bytes\":" << bytes
               << ",\"interval_seconds\":" << interval_seconds
               << ",\"fps\":" << std::setprecision(6) << fps
               << ",\"mib_per_second\":" << mib_per_second
               << ",\"no_frames\":" << (count == 0U ? "true" : "false")
               << ",\"ready_before_window\":" << (ready_before_window ? "true" : "false")
               << ",\"gaps\":" << gaps
               << ",\"duplicates\":" << duplicates
               << ",\"reordered\":" << reordered
               << ",\"first_index\":";
        if (has_sample) output << first_index;
        else output << "null";
        output << ",\"last_index\":";
        if (has_sample) output << last_index;
        else output << "null";
        output << ",\"first_receive_ns\":";
        if (has_sample) output << first_receive_ns;
        else output << "null";
        output << ",\"last_receive_ns\":";
        if (has_sample) output << last_receive_ns;
        else output << "null";
        output << ",\"latency_samples\":" << latencies.size()
               << ",\"latency_ns_p50\":";
        write_percentile(output, latencies, 50U);
        output << ",\"latency_ns_p90\":";
        write_percentile(output, latencies, 90U);
        output << ",\"latency_ns_p99\":";
        write_percentile(output, latencies, 99U);
        output << ",\"latency_ns_max\":";
        if (has_sample) output << max_latency_ns;
        else output << "null";
        output << ",\"checksum_fnv1a64\":\"0x" << std::hex << std::setw(16) << std::setfill('0')
               << checksum << "\"}";
        return output.str();
    }

  private:
    static constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    static constexpr std::size_t kMaximumLatencySamples = 10000U;

    static void increment(std::uint64_t& value, const char* error) {
        if (value == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error(error);
        ++value;
    }

    static void write_percentile(std::ostringstream& output,
                                 const std::vector<std::uint64_t>& sorted,
                                 std::uint32_t percentile) {
        if (sorted.empty()) {
            output << "null";
            return;
        }
        const auto rank = (sorted.size() * percentile + 99U) / 100U;
        output << sorted[std::max<std::size_t>(rank, 1U) - 1U];
    }

    void add_latency_sample(std::uint64_t latency) {
        if (latencies_.size() < kMaximumLatencySamples) {
            latencies_.push_back(latency);
            return;
        }

        // Deterministic reservoir sampling keeps percentile memory bounded and
        // representative if a source exceeds the expected 30 fps window.
        auto value = count_ + 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
        value ^= value >> 31U;
        const auto slot = value % count_;
        if (slot < kMaximumLatencySamples) latencies_[static_cast<std::size_t>(slot)] = latency;
    }

    void update_checksum(const std::uint8_t* data, std::size_t size) {
        const auto touch = [this](std::uint8_t byte) {
            checksum_ ^= byte;
            checksum_ *= kFnvPrime;
        };
        if (size == 0U) return;
        touch(data[0]);
        for (std::size_t index = 4096U; index < size; index += 4096U) touch(data[index]);
        if ((size - 1U) % 4096U != 0U) touch(data[size - 1U]);
    }

    mutable std::mutex mutex_;
    std::uint64_t count_ { 0U }, bytes_ { 0U }, gaps_ { 0U }, duplicates_ { 0U }, reordered_ { 0U };
    std::uint64_t previous_index_ { 0U }, first_index_ { 0U }, last_index_ { 0U };
    std::uint64_t max_latency_ns_ { 0U }, checksum_ { kFnvOffset };
    std::int64_t first_receive_ns_ { 0 }, last_receive_ns_ { 0 };
    bool has_sample_ { false }, ready_before_window_ { false };
    std::vector<std::uint64_t> latencies_;
};

} // namespace bench

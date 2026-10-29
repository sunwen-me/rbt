#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace transport_bench {

enum class Role { Publisher, Subscriber };
enum class Mode { Rbt, Shm, Loan };
enum class BenchCase { Imu, Cloud, Video };
enum class Backend { Rbt, Ros2 };

struct Options {
    Role role { Role::Publisher };
    Mode mode { Mode::Rbt };
    BenchCase bench_case { BenchCase::Imu };
    std::string topic;
    std::filesystem::path window_file;
    std::filesystem::path ready_file;
    std::filesystem::path output_file;
};

struct CaseSpec {
    std::size_t payload_bytes;
    std::int64_t period_ns;
};

struct Window {
    std::int64_t start_ns { 0 };
    std::int64_t end_ns { 0 };
};

struct Metrics {
    std::uint64_t published { 0 };
    std::uint64_t received { 0 };
    std::uint64_t invalid { 0 };
    std::uint64_t sequence_gaps { 0 };
    std::uint64_t duplicates { 0 };
    std::uint64_t regressions { 0 };
    std::uint64_t loaned_count { 0 };
    std::optional<std::uint64_t> first_sequence;
    std::optional<std::uint64_t> last_sequence;
    std::optional<std::uint64_t> previous_sequence;
    std::vector<std::int64_t> latency_ns;
    struct AddressProbe {
        std::uint64_t sequence;
        std::uintptr_t address;
    };
    std::vector<AddressProbe> address_probes;
};

inline constexpr std::int64_t kWindowWaitLimitNs = 20'000'000'000LL;
inline constexpr std::int64_t kShutdownDrainGraceNs = 100'000'000LL;

inline std::string_view name(Role role) noexcept {
    return role == Role::Publisher ? "pub" : "sub";
}

inline std::string_view name(Mode mode) noexcept {
    switch (mode) {
    case Mode::Rbt:
        return "rbt";
    case Mode::Shm:
        return "shm";
    case Mode::Loan:
        return "loan";
    }
    return "unknown";
}

inline std::string_view name(BenchCase bench_case) noexcept {
    switch (bench_case) {
    case BenchCase::Imu:
        return "imu";
    case BenchCase::Cloud:
        return "cloud";
    case BenchCase::Video:
        return "video";
    }
    return "unknown";
}

inline std::string_view name(Backend backend) noexcept {
    return backend == Backend::Rbt ? "rbt" : "ros2";
}

inline CaseSpec case_spec(BenchCase bench_case) {
    switch (bench_case) {
    case BenchCase::Imu:
        return { 256U, 5'000'000LL };
    case BenchCase::Cloud:
        return { 1'600'000U, 100'000'000LL };
    case BenchCase::Video:
        return { 6'220'800U, 33'333'333LL };
    }
    throw std::runtime_error("unknown benchmark case");
}

inline Role parse_role(std::string_view value) {
    if (value == "pub") {
        return Role::Publisher;
    }
    if (value == "sub") {
        return Role::Subscriber;
    }
    throw std::runtime_error("--role must be pub or sub");
}

inline Mode parse_mode(std::string_view value) {
    if (value == "rbt") {
        return Mode::Rbt;
    }
    if (value == "shm") {
        return Mode::Shm;
    }
    if (value == "loan") {
        return Mode::Loan;
    }
    throw std::runtime_error("--mode must be rbt, shm, or loan");
}

inline BenchCase parse_case(std::string_view value) {
    if (value == "imu") {
        return BenchCase::Imu;
    }
    if (value == "cloud") {
        return BenchCase::Cloud;
    }
    if (value == "video") {
        return BenchCase::Video;
    }
    throw std::runtime_error("--case must be imu, cloud, or video");
}

inline Options parse_options(int argc, char** argv, Backend backend) {
    Options options;
    options.mode = backend == Backend::Rbt ? Mode::Rbt : Mode::Shm;
    bool have_role = false;
    bool have_case = false;
    bool have_mode = false;
    bool have_topic = false;
    bool have_window = false;
    bool have_ready = false;
    bool have_output = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view flag(argv[index]);
        if (index + 1 >= argc) {
            throw std::runtime_error("missing value for " + std::string(flag));
        }
        const std::string_view value(argv[++index]);
        if (flag == "--role") {
            options.role = parse_role(value);
            have_role = true;
        } else if (flag == "--case") {
            options.bench_case = parse_case(value);
            have_case = true;
        } else if (flag == "--mode") {
            options.mode = parse_mode(value);
            have_mode = true;
        } else if (flag == "--topic") {
            options.topic = std::string(value);
            have_topic = true;
        } else if (flag == "--window-file") {
            options.window_file = std::string(value);
            have_window = true;
        } else if (flag == "--ready-file") {
            options.ready_file = std::string(value);
            have_ready = true;
        } else if (flag == "--output") {
            options.output_file = std::string(value);
            have_output = true;
        } else {
            throw std::runtime_error("unknown option: " + std::string(flag));
        }
    }

    if (!have_role || !have_case || !have_mode || !have_topic || !have_window || !have_ready || !have_output) {
        throw std::runtime_error(
            "required flags: --role --case --mode --topic --window-file --ready-file --output");
    }
    if (options.topic.empty() || options.window_file.empty() || options.ready_file.empty()
        || options.output_file.empty()) {
        throw std::runtime_error("topic and file paths must not be empty");
    }
    if ((backend == Backend::Rbt && options.mode != Mode::Rbt)
        || (backend == Backend::Ros2 && options.mode == Mode::Rbt)) {
        throw std::runtime_error(backend == Backend::Rbt
                                     ? "rbt_transport_bench requires --mode rbt"
                                     : "ros2_transport_bench requires --mode shm or --mode loan");
    }
    if (options.ready_file == options.output_file || options.ready_file == options.window_file
        || options.output_file == options.window_file) {
        throw std::runtime_error("ready, window, and output paths must be distinct");
    }
    return options;
}

inline std::int64_t monotonic_now_ns() {
    timespec value {};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        throw std::system_error(errno, std::generic_category(), "clock_gettime(CLOCK_MONOTONIC)");
    }
    return static_cast<std::int64_t>(value.tv_sec) * 1'000'000'000LL + value.tv_nsec;
}

inline timespec monotonic_timespec(std::int64_t value_ns) {
    if (value_ns < 0) {
        throw std::runtime_error("monotonic deadline is negative");
    }
    timespec value {};
    value.tv_sec = static_cast<time_t>(value_ns / 1'000'000'000LL);
    value.tv_nsec = static_cast<long>(value_ns % 1'000'000'000LL);
    return value;
}

inline void sleep_until_monotonic(std::int64_t deadline_ns) {
    const auto deadline = monotonic_timespec(deadline_ns);
    for (;;) {
        const auto result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
        if (result == 0) {
            return;
        }
        if (result != EINTR) {
            throw std::system_error(result, std::generic_category(), "clock_nanosleep");
        }
    }
}

inline std::string json_escape(std::string_view value) {
    std::ostringstream result;
    for (const auto character : value) {
        switch (character) {
        case '"':
            result << "\\\"";
            break;
        case '\\':
            result << "\\\\";
            break;
        case '\b':
            result << "\\b";
            break;
        case '\f':
            result << "\\f";
            break;
        case '\n':
            result << "\\n";
            break;
        case '\r':
            result << "\\r";
            break;
        case '\t':
            result << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                result << "?";
            } else {
                result << character;
            }
            break;
        }
    }
    return result.str();
}

inline void atomic_write(const std::filesystem::path& path, std::string_view content) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    auto temporary = path;
    temporary += ".tmp." + std::to_string(static_cast<unsigned long long>(::getpid()));
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open output file: " + temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot write output file: " + temporary.string());
        }
        output.close();
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

inline void write_ready(const Options& options,
                        Backend backend,
                        bool can_loan,
                        bool loan_enabled,
                        std::optional<std::string_view> datasharing_kind = std::nullopt,
                        std::optional<std::string_view> participant_transport = std::nullopt,
                        bool ready = true,
                        std::string_view error = {}) {
    const auto spec = case_spec(options.bench_case);
    std::ostringstream json;
    json << "{\"ready\":" << (ready ? "true" : "false")
         << ",\"role\":\"" << name(options.role) << "\""
         << ",\"backend\":\"" << name(backend) << "\""
         << ",\"mode\":\"" << name(options.mode) << "\""
         << ",\"case\":\"" << name(options.bench_case) << "\""
         << ",\"payload_bytes\":" << spec.payload_bytes
         << ",\"can_loan\":" << (can_loan ? "true" : "false")
         << ",\"loan_enabled\":" << (loan_enabled ? "true" : "false")
         << ",\"datasharing_kind\":";
    if (datasharing_kind.has_value()) {
        json << "\"" << json_escape(*datasharing_kind) << "\"";
    } else {
        json << "null";
    }
    json << ",\"participant_transport\":";
    if (participant_transport.has_value()) {
        json << "\"" << json_escape(*participant_transport) << "\"";
    } else {
        json << "null";
    }
    if (!error.empty()) {
        json << ",\"error\":\"" << json_escape(error) << "\"";
    }
    json << ",\"pid\":" << static_cast<unsigned long long>(::getpid()) << "}\n";
    atomic_write(options.ready_file, json.str());
}

inline Window read_window_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot open window file: " + path.string());
    }
    Window window;
    std::string extra;
    if (!(input >> window.start_ns >> window.end_ns) || (input >> extra)) {
        throw std::runtime_error("window file must contain exactly start_ns and end_ns integers");
    }
    if (window.start_ns < 0 || window.end_ns <= window.start_ns) {
        throw std::runtime_error("window file has an invalid monotonic interval");
    }
    return window;
}

inline Window wait_for_window_file(const std::filesystem::path& path) {
    const auto deadline = monotonic_now_ns() + kWindowWaitLimitNs;
    for (;;) {
        std::error_code error;
        const bool exists = std::filesystem::exists(path, error);
        if (error) {
            throw std::system_error(error, "checking window file");
        }
        if (exists) {
            return read_window_file(path);
        }
        if (monotonic_now_ns() >= deadline) {
            throw std::runtime_error("timed out after 20 seconds waiting for window file: " + path.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

inline void record_sequence(Metrics& metrics, std::uint64_t sequence) {
    if (!metrics.first_sequence.has_value()) {
        metrics.first_sequence = sequence;
    }
    if (metrics.previous_sequence.has_value()) {
        const auto previous = metrics.previous_sequence.value();
        if (sequence == previous) {
            ++metrics.duplicates;
        } else if (sequence < previous) {
            ++metrics.regressions;
        } else if (sequence - previous > 1U) {
            metrics.sequence_gaps += sequence - previous - 1U;
        }
    }
    metrics.previous_sequence = sequence;
    metrics.last_sequence = sequence;
}

inline void record_address_probe(Metrics& metrics, std::uint64_t sequence, std::uintptr_t address) {
    if (metrics.address_probes.size() >= 8U) {
        return;
    }
    const auto exists = std::any_of(metrics.address_probes.begin(), metrics.address_probes.end(),
                                    [sequence](const Metrics::AddressProbe& probe) {
                                        return probe.sequence == sequence;
                                    });
    if (!exists) {
        metrics.address_probes.push_back({ sequence, address });
    }
}

template <typename Message>
inline std::size_t payload_size(const Message& message) {
    return std::size(message.data);
}

template <typename Message>
inline void fill_message(Message& message, std::uint64_t sequence, std::size_t expected_bytes) {
    const auto bytes = payload_size(message);
    if (bytes != expected_bytes) {
        throw std::runtime_error("generated message payload size does not match case configuration");
    }
    auto* data = std::data(message.data);
    const auto value = static_cast<unsigned char>((sequence % 251U) + 1U);
    std::memset(data, value, bytes);
    message.sequence = sequence;
    message.sent_ns = static_cast<decltype(message.sent_ns)>(monotonic_now_ns());
}

template <typename Message>
inline bool payload_valid(const Message& message, std::size_t expected_bytes) {
    const auto bytes = payload_size(message);
    if (bytes != expected_bytes || bytes == 0U) {
        return false;
    }
    const auto* data = std::data(message.data);
    const auto expected = static_cast<unsigned char>((message.sequence % 251U) + 1U);
    constexpr std::size_t kValidationStride = 4096U;
    for (std::size_t offset = 0U; offset < bytes; offset += kValidationStride) {
        if (data[offset] != expected) {
            return false;
        }
    }
    return data[bytes - 1U] == expected;
}

template <typename Message>
inline bool record_received(Metrics& metrics,
                            const Message& message,
                            std::int64_t receive_ns,
                            const Window& window,
                            std::size_t expected_bytes,
                            std::uintptr_t address,
                            bool loaned) {
    const auto sent_ns = static_cast<std::int64_t>(message.sent_ns);
    if (receive_ns < window.start_ns || receive_ns >= window.end_ns || sent_ns < window.start_ns
        || sent_ns >= window.end_ns) {
        return false;
    }
    ++metrics.received;
    record_address_probe(metrics, message.sequence, address);
    if (!payload_valid(message, expected_bytes) || receive_ns < sent_ns) {
        ++metrics.invalid;
    } else {
        metrics.latency_ns.emplace_back(receive_ns - sent_ns);
    }
    record_sequence(metrics, message.sequence);
    if (loaned) {
        ++metrics.loaned_count;
    }
    return true;
}

inline void record_published(Metrics& metrics,
                             std::uint64_t sequence,
                             std::int64_t sent_ns,
                             const Window& window,
                             std::uintptr_t address,
                             bool loaned) {
    if (sent_ns < window.start_ns || sent_ns >= window.end_ns) {
        return;
    }
    ++metrics.published;
    record_address_probe(metrics, sequence, address);
    record_sequence(metrics, sequence);
    if (loaned) {
        ++metrics.loaned_count;
    }
}

inline std::optional<double> percentile_us(const std::vector<std::int64_t>& samples, double quantile) {
    if (samples.empty()) {
        return std::nullopt;
    }
    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted.size())));
    const auto index = std::max<std::size_t>(1U, rank) - 1U;
    return static_cast<double>(sorted[index]) / 1000.0;
}

inline void write_output(const Options& options,
                         Backend backend,
                         const Window& window,
                         const Metrics& metrics,
                         std::string_view error = {}) {
    const auto spec = case_spec(options.bench_case);
    const auto p50 = percentile_us(metrics.latency_ns, 0.50);
    const auto p99 = percentile_us(metrics.latency_ns, 0.99);
    std::ostringstream json;
    json << "{\"role\":\"" << name(options.role) << "\""
         << ",\"backend\":\"" << name(backend) << "\""
         << ",\"mode\":\"" << name(options.mode) << "\""
         << ",\"case\":\"" << name(options.bench_case) << "\""
         << ",\"payload_bytes\":" << spec.payload_bytes
         << ",\"start_ns\":" << window.start_ns
         << ",\"end_ns\":" << window.end_ns;
    if (options.role == Role::Publisher) {
        json << ",\"published\":" << metrics.published;
    } else {
        json << ",\"received\":" << metrics.received;
    }
    json << ",\"invalid\":" << metrics.invalid
         << ",\"sequence_gaps\":" << metrics.sequence_gaps
         << ",\"duplicates\":" << metrics.duplicates
         << ",\"regressions\":" << metrics.regressions
         << ",\"first_sequence\":";
    if (metrics.first_sequence.has_value()) {
        json << metrics.first_sequence.value();
    } else {
        json << "null";
    }
    json << ",\"last_sequence\":";
    if (metrics.last_sequence.has_value()) {
        json << metrics.last_sequence.value();
    } else {
        json << "null";
    }
    json << ",\"latency_p50_us\":";
    if (p50.has_value()) {
        json << std::fixed << std::setprecision(3) << p50.value();
    } else {
        json << "null";
    }
    json << ",\"latency_p99_us\":";
    if (p99.has_value()) {
        json << std::fixed << std::setprecision(3) << p99.value();
    } else {
        json << "null";
    }
    json << ",\"loaned_count\":" << metrics.loaned_count;
    json << ",\"address_probes\":[";
    for (std::size_t index = 0U; index < metrics.address_probes.size(); ++index) {
        if (index != 0U) {
            json << ',';
        }
        const auto& probe = metrics.address_probes[index];
        json << "{\"sequence\":" << probe.sequence
             << ",\"address\":" << static_cast<unsigned long long>(probe.address) << '}';
    }
    json << ']';
    if (!error.empty()) {
        json << ",\"error\":\"" << json_escape(error) << "\"";
    }
    json << "}\n";
    atomic_write(options.output_file, json.str());
}

template <typename PublishOne>
inline void run_publish_schedule(const Options& options,
                                 const Window& window,
                                 Metrics& metrics,
                                 bool loaned,
                                 PublishOne&& publish_one) {
    const auto spec = case_spec(options.bench_case);
    auto deadline = monotonic_now_ns();
    std::uint64_t sequence = 0U;
    while (deadline < window.end_ns) {
        sleep_until_monotonic(deadline);
        if (monotonic_now_ns() >= window.end_ns) {
            break;
        }
        std::uintptr_t address = 0U;
        const auto sent_ns = publish_one(sequence, window.end_ns, spec.payload_bytes, address);
        if (!sent_ns.has_value()) {
            break;
        }
        record_published(metrics, sequence, sent_ns.value(), window, address, loaned);
        ++sequence;
        if (deadline > std::numeric_limits<std::int64_t>::max() - spec.period_ns) {
            throw std::runtime_error("publisher deadline overflow");
        }
        deadline += spec.period_ns;
    }
    // Keep endpoint mappings available through the full measured window and
    // subscriber shutdown drain; this wait does not publish or count samples.
    sleep_until_monotonic(window.end_ns + kShutdownDrainGraceNs);
}

} // namespace transport_bench

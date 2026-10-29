#include <rbt/time.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct ReadlinkResponse {
    bool fail { false };
    std::string text;
};

std::vector<ReadlinkResponse> readlink_responses;
std::size_t readlink_calls = 0U;
bool scripted_readlink = false;

void set_readlink_script(std::initializer_list<ReadlinkResponse> responses) {
    readlink_responses.assign(responses.begin(), responses.end());
    readlink_calls = 0U;
    scripted_readlink = true;
}

std::size_t clear_readlink_script() {
    const auto calls = readlink_calls;
    scripted_readlink = false;
    readlink_responses.clear();
    readlink_calls = 0U;
    return calls;
}

std::string namespace_link(std::uint64_t inode) {
    return "time:[" + std::to_string(inode) + "]";
}

std::uint64_t namespace_inode(const rbt::time::HostClock& clock) {
    const auto& bytes = clock.monotonic_identity().source_id().bytes();
    std::uint64_t inode = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        inode |= static_cast<std::uint64_t>(bytes[24U + index]) << (index * 8U);
    }
    return inode;
}

bool expect_error(const rbt::time::HostClock& clock,
                  std::initializer_list<ReadlinkResponse> responses,
                  rbt::time::TimeError expected,
                  std::size_t expected_calls) {
    set_readlink_script(responses);
    auto result = clock.now_monotonic();
    const auto calls = clear_readlink_script();
    return !result && result.error() == expected && calls == expected_calls;
}

bool test_real_clock_move_and_fork() {
    auto created = rbt::time::HostClock::create();
    if (!created) {
        return false;
    }
    rbt::time::HostClock clock(std::move(created).value());
    auto original = clock.now_monotonic();
    if (!original || original.value().ns <= 0
        || original.value().clock.source_id() != clock.monotonic_identity().source_id()) {
        return false;
    }

    rbt::time::HostClock moved(std::move(clock));
    auto after_move = moved.now_monotonic();
    if (!after_move || after_move.value().ns <= 0
        || after_move.value().clock.source_id() != moved.monotonic_identity().source_id()) {
        return false;
    }

    const auto child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        const auto child_sample = moved.now_monotonic();
        ::_exit(child_sample ? 0 : 1);
    }
    int child_status = 0;
    return ::waitpid(child, &child_status, 0) == child
           && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
}

bool test_namespace_read_checks() {
    auto created = rbt::time::HostClock::create();
    if (!created) {
        return false;
    }
    auto clock = std::move(created).value();
    const auto inode = namespace_inode(clock);
    const auto matching = namespace_link(inode);
    const auto mismatching = namespace_link(inode == UINT64_MAX ? inode - 1U : inode + 1U);

    set_readlink_script({ { false, matching }, { false, matching } });
    auto success = clock.now_monotonic();
    const auto successful_calls = clear_readlink_script();
    if (!success || success.value().clock.source_id() != clock.monotonic_identity().source_id()
        || successful_calls != 2U) {
        return false;
    }

    if (!expect_error(clock, { { false, mismatching } }, rbt::time::TimeError::TimeSourceReset, 1U)
        || !expect_error(clock, { { false, matching }, { false, mismatching } },
                         rbt::time::TimeError::TimeSourceReset, 2U)
        || !expect_error(clock, { { true, {} } }, rbt::time::TimeError::ClockUnavailable, 1U)
        || !expect_error(clock, { { false, matching }, { true, {} } },
                         rbt::time::TimeError::ClockUnavailable, 2U)
        || !expect_error(clock, { { false, "time:[not-an-inode]" } },
                         rbt::time::TimeError::ClockUnavailable, 1U)
        || !expect_error(clock, { { false, matching }, { false, "time:[12x]" } },
                         rbt::time::TimeError::ClockUnavailable, 2U)
        || !expect_error(clock, { { false, "time:[18446744073709551616]" } },
                         rbt::time::TimeError::ClockUnavailable, 1U)) {
        return false;
    }

    const std::string truncated(80U, '7');
    return expect_error(clock, { { false, truncated } }, rbt::time::TimeError::ClockUnavailable, 1U);
}

} // namespace

extern "C" ssize_t readlink(const char* path, char* buffer, std::size_t size) {
    if (scripted_readlink && std::strcmp(path, "/proc/thread-self/ns/time") == 0) {
        const auto call = readlink_calls++;
        if (call >= readlink_responses.size() || readlink_responses[call].fail) {
            errno = EIO;
            return -1;
        }
        const auto& response = readlink_responses[call].text;
        const auto copied = response.size() < size ? response.size() : size;
        std::memcpy(buffer, response.data(), copied);
        return static_cast<ssize_t>(copied);
    }
    return static_cast<ssize_t>(::syscall(SYS_readlinkat, AT_FDCWD, path, buffer, size));
}

int main() {
    if (!test_real_clock_move_and_fork()) {
        std::cerr << "host_clock_namespace_tests: real clock, move, or fork failed\n";
        return 1;
    }
    if (!test_namespace_read_checks()) {
        std::cerr << "host_clock_namespace_tests: scripted namespace checks failed\n";
        return 2;
    }
    return 0;
}

#pragma once

#include <cstdint>
#include <string>
#include <span>

namespace rbt_dm_imu {

enum class PollResult {
    Readable,
    Timeout,
    Interrupted,
    Disconnected,
};

class SerialPort {
public:
    SerialPort() = default;
    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;
    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;
    ~SerialPort();

    [[nodiscard]] bool open(const std::string& path, std::uint32_t baud) noexcept;
    void close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] PollResult wait_readable(int timeout_ms) noexcept;
    [[nodiscard]] int read(std::span<std::uint8_t> buffer) noexcept;
    [[nodiscard]] bool write_all(std::span<const std::uint8_t> bytes,
                                 bool (*should_stop)()) noexcept;

private:
    int fd_ = -1;
};

} // namespace rbt_dm_imu

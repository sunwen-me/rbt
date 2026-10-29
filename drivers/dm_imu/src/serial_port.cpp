#include <rbt_dm_imu/serial_port.hpp>

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace rbt_dm_imu {
namespace {

[[nodiscard]] speed_t baud_speed(std::uint32_t baud) noexcept {
    return baud == 921600U ? B921600 : static_cast<speed_t>(0);
}

[[nodiscard]] int wait_fd(int fd, short events, int timeout_ms) noexcept {
    pollfd descriptor {fd, events, 0};
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(std::clamp(timeout_ms, 0, 20));
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = now >= deadline
                                  ? 0
                                  : static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - now)
                                                         .count());
        const auto result = ::poll(&descriptor, 1U, remaining);
        if (result < 0 && errno == EINTR) {
            return -2;
        }
        if (result <= 0) {
            return result;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return -3;
        }
        if ((descriptor.revents & events) != 0) {
            return 1;
        }
    }
}

} // namespace

SerialPort::SerialPort(SerialPort&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

SerialPort& SerialPort::operator=(SerialPort&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

SerialPort::~SerialPort() {
    close();
}

bool SerialPort::open(const std::string& path, std::uint32_t baud) noexcept {
    close();
    const auto speed = baud_speed(baud);
    if (speed == 0) {
        return false;
    }
    fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        return false;
    }

    termios settings {};
    if (::tcgetattr(fd_, &settings) != 0) {
        close();
        return false;
    }
    settings.c_iflag &= static_cast<tcflag_t>(~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL
                                                | IXON | IXOFF | IXANY));
    settings.c_oflag &= static_cast<tcflag_t>(~OPOST);
    settings.c_lflag &= static_cast<tcflag_t>(~(ECHO | ECHONL | ICANON | ISIG | IEXTEN));
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= static_cast<tcflag_t>(~(CSIZE | PARENB | CSTOPB | CRTSCTS));
    settings.c_cflag |= CS8;
    settings.c_cc[VMIN] = 0;
    settings.c_cc[VTIME] = 0;
    if (::cfsetispeed(&settings, speed) != 0 || ::cfsetospeed(&settings, speed) != 0
        || ::tcsetattr(fd_, TCSANOW, &settings) != 0) {
        close();
        return false;
    }
    (void)::tcflush(fd_, TCIOFLUSH);
    return true;
}

void SerialPort::close() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

bool SerialPort::is_open() const noexcept {
    return fd_ >= 0;
}

PollResult SerialPort::wait_readable(int timeout_ms) noexcept {
    if (fd_ < 0) {
        return PollResult::Disconnected;
    }
    const auto result = wait_fd(fd_, POLLIN, timeout_ms);
    if (result == 1) {
        return PollResult::Readable;
    }
    if (result == 0) {
        return PollResult::Timeout;
    }
    return result == -2 ? PollResult::Interrupted : PollResult::Disconnected;
}

int SerialPort::read(std::span<std::uint8_t> buffer) noexcept {
    if (fd_ < 0) {
        return -1;
    }
    const auto result = ::read(fd_, buffer.data(), buffer.size());
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return 0;
    }
    return static_cast<int>(result);
}

bool SerialPort::write_all(std::span<const std::uint8_t> bytes, bool (*should_stop)()) noexcept {
    if (fd_ < 0) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    std::size_t offset = 0U;
    while (offset < bytes.size() && !should_stop()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return false;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        const auto result = wait_fd(fd_, POLLOUT, static_cast<int>(remaining.count()));
        if (result == -2) {
            continue;
        }
        if (result != 1) {
            return false;
        }
        const auto written = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            continue;
        }
        if (written <= 0) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return offset == bytes.size();
}

} // namespace rbt_dm_imu

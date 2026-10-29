#include "ddsm_driver/serial_comm.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace ddsm_driver
{

SerialComm::SerialComm(const std::string & port, uint32_t baudrate)
: port_(port), baudrate_(baudrate), fd_(-1)
{
}

SerialComm::~SerialComm()
{
  close();
}

bool SerialComm::open()
{
  if (fd_ >= 0) {
    close();
  }

  fd_ = ::open(port_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd_ < 0) {
    return false;
  }

  // Configure serial port
  struct termios tty;
  std::memset(&tty, 0, sizeof(tty));

  if (tcgetattr(fd_, &tty) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // Set baud rate
  speed_t speed;
  switch (baudrate_) {
    case 9600:   speed = B9600;   break;
    case 19200:  speed = B19200;  break;
    case 38400:  speed = B38400;  break;
    case 57600:  speed = B57600;  break;
    case 115200: speed = B115200; break;
    case 230400: speed = B230400; break;
    case 460800: speed = B460800; break;
    case 921600: speed = B921600; break;
    default:     speed = B115200; break;
  }
  cfsetispeed(&tty, speed);
  cfsetospeed(&tty, speed);

  // 8N1, no flow control, enable receiver
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
  tty.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
  tty.c_cflag |= (CLOCAL | CREAD);

  // Raw input mode
  tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP |
                    INLCR | IGNCR | ICRNL | IXON);
  tty.c_oflag &= ~OPOST;
  tty.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

  // Let select() manage read timeouts explicitly.
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 0;

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  // Flush any existing data
  tcflush(fd_, TCIOFLUSH);
  return true;
}

void SerialComm::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool SerialComm::isOpen() const
{
  return fd_ >= 0;
}

bool SerialComm::writeBytes(const uint8_t * data, size_t size)
{
  return writeAll(data, size, 20);
}

bool SerialComm::writeAll(const uint8_t * data, size_t size, int timeout_ms)
{
  if (fd_ < 0 || data == nullptr || size == 0 || timeout_ms <= 0) {
    return false;
  }

  size_t total_written = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (total_written < size) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }

    const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor{fd_, POLLOUT, 0};
    const int ready = ::poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (ready == 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      return false;
    }

    const ssize_t n = ::write(fd_, data + total_written, size - total_written);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      return false;
    }
    if (n == 0) {
      return false;
    }
    total_written += static_cast<size_t>(n);
  }

  return true;
}

std::vector<uint8_t> SerialComm::readSome(size_t max_size, int timeout_ms)
{
  std::vector<uint8_t> response;
  if (fd_ < 0 || max_size == 0 || timeout_ms <= 0) {
    return response;
  }

  pollfd descriptor{fd_, POLLIN, 0};
  int ready = 0;
  do {
    ready = ::poll(&descriptor, 1, timeout_ms);
  } while (ready < 0 && errno == EINTR);

  if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
    return response;
  }

  response.resize(std::min<size_t>(max_size, 256));
  const ssize_t n = ::read(fd_, response.data(), response.size());
  if (n <= 0) {
    response.clear();
    return response;
  }
  response.resize(static_cast<size_t>(n));
  return response;
}

void SerialComm::flushInput()
{
  if (fd_ >= 0) {
    tcflush(fd_, TCIFLUSH);
  }
}

}  // namespace ddsm_driver

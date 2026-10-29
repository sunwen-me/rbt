#ifndef DDSM_DRIVER__SERIAL_COMM_HPP_
#define DDSM_DRIVER__SERIAL_COMM_HPP_

#include <string>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ddsm_driver
{

class SerialComm
{
public:
  SerialComm(const std::string & port, uint32_t baudrate = 115200);
  ~SerialComm();

  bool open();
  void close();
  bool isOpen() const;

  bool writeBytes(const uint8_t * data, size_t size);
  std::vector<uint8_t> readSome(size_t max_size, int timeout_ms = 20);
  void flushInput();

private:
  bool writeAll(const uint8_t * data, size_t size, int timeout_ms);

  std::string port_;
  uint32_t baudrate_;
  int fd_;
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__SERIAL_COMM_HPP_

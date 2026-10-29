#include <array>
#include <chrono>
#include <cstdint>
#include <poll.h>
#include <pty.h>
#include <string>
#include <thread>
#include <unistd.h>

#include "ddsm_driver/ddsm_motor.hpp"
#include "ddsm_driver/ddsm_protocol.hpp"
#include "ddsm_driver/serial_comm.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

namespace
{

struct PseudoTerminal
{
  int master{-1};
  std::string slave_name;

  PseudoTerminal()
  {
    int slave = -1;
    std::array<char, 128> name{};
    DDSM_CHECK(openpty(&master, &slave, name.data(), nullptr, nullptr) == 0);
    slave_name = name.data();
    ::close(slave);
  }

  ~PseudoTerminal()
  {
    if (master >= 0) {
      ::close(master);
    }
  }
};

bool readExact(int fd, uint8_t * data, size_t size)
{
  size_t offset = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (offset < size && std::chrono::steady_clock::now() < deadline) {
    pollfd descriptor{fd, POLLIN, 0};
    if (::poll(&descriptor, 1, 10) <= 0) {
      continue;
    }
    const ssize_t n = ::read(fd, data + offset, size - offset);
    if (n > 0) {
      offset += static_cast<size_t>(n);
    }
  }
  return offset == size;
}

void putU16(uint8_t * data, uint16_t value)
{
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value & 0xFF);
}

HatFeedbackFrame makeHatFeedback(uint8_t type, uint16_t sequence, int left_speed, int right_speed)
{
  HatFeedbackFrame frame{};
  frame[0] = HAT_MAGIC_0;
  frame[1] = HAT_MAGIC_1;
  frame[2] = HAT_PROTOCOL_VERSION;
  frame[3] = type;
  putU16(frame.data() + 4, sequence);
  frame[6] = 0x03;
  frame[7] = 115;
  frame[8] = 1;
  frame[9] = HAT_MOTOR_VALID;
  frame[10] = 2;
  putU16(frame.data() + 14, static_cast<uint16_t>(left_speed));
  frame[18] = 2;
  frame[19] = HAT_MOTOR_VALID;
  frame[20] = 2;
  putU16(frame.data() + 24, static_cast<uint16_t>(right_speed));
  frame[31] = crc8Maxim(frame.data(), frame.size() - 1);
  return frame;
}

void test_hat_binary_pair_exchange_and_resynchronization()
{
  PseudoTerminal pty;
  SerialComm serial(pty.slave_name, 115200);
  DDSM_CHECK(serial.open());
  HatBinaryMotorTransport motor(serial);

  std::thread configure_responder([&pty]() {
      HatCommandFrame command{};
      DDSM_CHECK(readExact(pty.master, command.data(), command.size()));
      DDSM_CHECK(command[3] == HAT_CONFIG_COMMAND);
      DDSM_CHECK(command[6] == 1 && command[7] == 2);
      DDSM_CHECK(command[14] == 115);
      DDSM_CHECK(command[16] == 0x03 && command[17] == 0xE8);
      DDSM_CHECK(command[19] == crc8Maxim(command.data(), command.size() - 1));
      const HatFeedbackFrame response = makeHatFeedback(HAT_CONFIG_RESPONSE, 1, 0, 0);
      DDSM_CHECK(::write(pty.master, response.data(), response.size()) ==
        static_cast<ssize_t>(response.size()));
    });
  DDSM_CHECK(motor.configure(115, 1000, 1, 2));
  configure_responder.join();

  std::thread control_responder([&pty]() {
      HatCommandFrame command{};
      DDSM_CHECK(readExact(pty.master, command.data(), command.size()));
      DDSM_CHECK(command[3] == HAT_CONTROL_COMMAND);
      DDSM_CHECK(command[8] == 0xFF && command[9] == 0xCE);
      DDSM_CHECK(command[10] == 0x00 && command[11] == 0x32);

      const std::array<uint8_t, 3> noise{0x99, 0xA5, 0x00};
      DDSM_CHECK(::write(pty.master, noise.data(), noise.size()) ==
        static_cast<ssize_t>(noise.size()));
      HatFeedbackFrame bad = makeHatFeedback(HAT_CONTROL_RESPONSE, 2, -50, 50);
      bad[31] ^= 0x01;
      DDSM_CHECK(::write(pty.master, bad.data(), bad.size()) ==
        static_cast<ssize_t>(bad.size()));
      const HatFeedbackFrame response = makeHatFeedback(HAT_CONTROL_RESPONSE, 2, -50, 50);
      DDSM_CHECK(::write(pty.master, response.data(), 11) == 11);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      DDSM_CHECK(::write(pty.master, response.data() + 11, response.size() - 11) ==
        static_cast<ssize_t>(response.size() - 11));
    });
  const MotorFeedbackPair feedback = motor.exchangePair(1, -50, 2, 50);
  control_responder.join();
  DDSM_CHECK(feedback.valid);
  DDSM_CHECK(feedback.left.valid && feedback.left.speed == -50);
  DDSM_CHECK(feedback.right.valid && feedback.right.speed == 50);
}

void test_rs485_pair_exchange_resynchronizes_after_echo_and_noise()
{
  PseudoTerminal pty;
  SerialComm serial(pty.slave_name, 115200);
  DDSM_CHECK(serial.open());
  Rs485MotorTransport motor(serial);

  std::thread responder([&pty]() {
      for (uint8_t id = 1; id <= 2; ++id) {
        DdsmFrame command{};
        DDSM_CHECK(readExact(pty.master, command.data(), command.size()));
        if (id == 1) {
          DDSM_CHECK(::write(pty.master, command.data(), command.size()) ==
            static_cast<ssize_t>(command.size()));
          const std::array<uint8_t, 3> noise{0x99, 0x00, 0x55};
          DDSM_CHECK(::write(pty.master, noise.data(), noise.size()) ==
            static_cast<ssize_t>(noise.size()));
        }

        DdsmFrame feedback{};
        feedback[0] = id;
        feedback[1] = 2;
        feedback[4] = 0x00;
        feedback[5] = static_cast<uint8_t>(id == 1 ? 50 : 60);
        feedback[9] = crc8Maxim(feedback.data(), 9);
        DDSM_CHECK(::write(pty.master, feedback.data(), feedback.size()) ==
          static_cast<ssize_t>(feedback.size()));
      }
    });

  const MotorFeedbackPair feedback = motor.exchangePair(1, 50, 2, 60, 0);
  responder.join();
  DDSM_CHECK(feedback.left.valid && feedback.left.speed == 50);
  DDSM_CHECK(feedback.right.valid && feedback.right.speed == 60);
}

}  // namespace

int main()
{
  test_hat_binary_pair_exchange_and_resynchronization();
  test_rs485_pair_exchange_resynchronizes_after_echo_and_noise();
  return 0;
}

#include "ddsm_driver/ddsm_motor.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace ddsm_driver
{

HatBinaryMotorTransport::HatBinaryMotorTransport(SerialComm & serial)
: serial_(serial)
{
}

uint16_t HatBinaryMotorTransport::nextSequence()
{
  sequence_ = static_cast<uint16_t>(sequence_ + 1);
  return sequence_;
}

bool HatBinaryMotorTransport::configure(
  int motor_type, int heartbeat_ms, int left_id, int right_id)
{
  if ((motor_type != 115 && motor_type != 210) || heartbeat_ms > 65534) {
    return false;
  }

  serial_.flushInput();
  rx_buffer_.clear();
  const uint16_t sequence = nextSequence();
  const HatCommandFrame frame = buildHatConfigCommand(
    sequence, motor_type, heartbeat_ms, left_id, right_id);
  if (!serial_.writeBytes(frame.data(), frame.size())) {
    return false;
  }

  const MotorFeedbackPair response = readPair(
    sequence, left_id, right_id, HAT_CONFIG_RESPONSE, 80);
  return response.valid && response.bridge_error == 0 &&
    response.left.valid && response.left.error == 0 &&
    response.right.valid && response.right.error == 0;
}

MotorFeedbackPair HatBinaryMotorTransport::exchangePair(
  int left_id, int left_cmd, int right_id, int right_cmd, int act)
{
  const uint16_t sequence = nextSequence();
  const HatCommandFrame frame = buildHatControlCommand(
    sequence, left_id, left_cmd, right_id, right_cmd, act, false);
  if (!serial_.writeBytes(frame.data(), frame.size())) {
    return MotorFeedbackPair{};
  }
  return readPair(sequence, left_id, right_id, HAT_CONTROL_RESPONSE, 25);
}

bool HatBinaryMotorTransport::stopPair(int left_id, int right_id)
{
  const uint16_t sequence = nextSequence();
  const HatCommandFrame frame = buildHatControlCommand(
    sequence, left_id, 0, right_id, 0, 3, true);
  if (!serial_.writeBytes(frame.data(), frame.size())) {
    return false;
  }
  const MotorFeedbackPair response = readPair(
    sequence, left_id, right_id, HAT_CONTROL_RESPONSE, 25);
  return response.valid && response.left.valid && response.left.error == 0 &&
    response.right.valid && response.right.error == 0;
}

MotorFeedbackPair HatBinaryMotorTransport::readPair(
  uint16_t sequence, int left_id, int right_id, uint8_t response_type, int timeout_ms)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    while (rx_buffer_.size() >= HatFeedbackFrame{}.size()) {
      if (rx_buffer_[0] != HAT_MAGIC_0 || rx_buffer_[1] != HAT_MAGIC_1) {
        rx_buffer_.pop_front();
        continue;
      }

      HatFeedbackFrame frame{};
      std::copy_n(rx_buffer_.begin(), frame.size(), frame.begin());
      if (crc8Maxim(frame.data(), frame.size() - 1) != frame.back() ||
        frame[2] != HAT_PROTOCOL_VERSION)
      {
        rx_buffer_.pop_front();
        continue;
      }
      for (size_t i = 0; i < frame.size(); ++i) {
        rx_buffer_.pop_front();
      }

      MotorFeedbackPair result = parseHatFeedback(
        frame, sequence, left_id, right_id, response_type);
      if (result.valid) {
        return result;
      }
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
    const auto bytes = serial_.readSome(64, std::max(1, static_cast<int>(remaining.count())));
    for (const uint8_t byte : bytes) {
      rx_buffer_.push_back(byte);
      if (rx_buffer_.size() > 256) {
        rx_buffer_.pop_front();
      }
    }
  }
  return MotorFeedbackPair{};
}

Rs485MotorTransport::Rs485MotorTransport(SerialComm & serial)
: serial_(serial)
{
}

bool Rs485MotorTransport::configure(int motor_type, int heartbeat_ms, int left_id, int right_id)
{
  (void)heartbeat_ms;
  if (motor_type != 115 || !setMode(left_id, 2)) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  if (!setMode(right_id, 2)) {
    return false;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  const MotorFeedbackPair response = exchangePair(left_id, 0, right_id, 0, 0);
  return response.left.valid && response.left.error == 0 &&
    response.right.valid && response.right.error == 0;
}

bool Rs485MotorTransport::setMode(int id, int mode)
{
  const DdsmFrame frame = buildRs485ModeCommand(id, mode);
  return serial_.writeBytes(frame.data(), frame.size());
}

MotorFeedback Rs485MotorTransport::exchangeSingle(int id, int cmd, int act, bool brake)
{
  const DdsmFrame frame = buildRs485SpeedCommand(id, cmd, act, brake);
  if (!serial_.writeBytes(frame.data(), frame.size())) {
    return MotorFeedback{};
  }
  return readFrame(id, 8, false);
}

MotorFeedbackPair Rs485MotorTransport::exchangePair(
  int left_id, int left_cmd, int right_id, int right_cmd, int act)
{
  MotorFeedbackPair result;
  result.left = exchangeSingle(left_id, left_cmd, act);
  result.right = exchangeSingle(right_id, right_cmd, act);
  result.valid = true;
  return result;
}

bool Rs485MotorTransport::stopPair(int left_id, int right_id)
{
  const MotorFeedback left = exchangeSingle(left_id, 0, 3, true);
  const MotorFeedback right = exchangeSingle(right_id, 0, 3, true);
  return left.valid && left.error == 0 && right.valid && right.error == 0;
}

MotorFeedback Rs485MotorTransport::readFrame(
  int expected_id, int timeout_ms, bool other_feedback)
{
  const auto deadline =
    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    while (rx_buffer_.size() >= DdsmFrame{}.size()) {
      DdsmFrame frame{};
      std::copy_n(rx_buffer_.begin(), frame.size(), frame.begin());

      const bool crc_valid = crc8Maxim(frame.data(), 9) == frame[9];
      const bool semantic_valid = frame[0] >= 1 && frame[0] <= 253 &&
        frame[1] >= 1 && frame[1] <= 3;
      if (crc_valid && semantic_valid) {
        for (size_t i = 0; i < frame.size(); ++i) {
          rx_buffer_.pop_front();
        }
        if (expected_id >= 0 && frame[0] != expected_id) {
          continue;
        }
        return other_feedback ?
               parseRs485OtherFeedback(frame) : parseRs485MotorFeedback(frame);
      }

      const bool command_echo = crc_valid &&
        (frame[1] == 0x64 || frame[1] == 0x74 || frame[1] == 0xA0);
      if (command_echo) {
        for (size_t i = 0; i < frame.size(); ++i) {
          rx_buffer_.pop_front();
        }
      } else {
        rx_buffer_.pop_front();
      }
    }

    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline - std::chrono::steady_clock::now());
    const auto bytes = serial_.readSome(64, std::max(1, static_cast<int>(remaining.count())));
    for (const uint8_t byte : bytes) {
      rx_buffer_.push_back(byte);
      if (rx_buffer_.size() > 256) {
        rx_buffer_.pop_front();
      }
    }
  }
  return MotorFeedback{};
}

}  // namespace ddsm_driver

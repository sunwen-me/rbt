#include "ddsm_driver/ddsm_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ddsm_driver
{

int rpmToMotorCommand(int motor_type, double rpm)
{
  if (std::isnan(rpm)) {
    return 0;
  }
  const double scale = motor_type == 210 ? 10.0 : 1.0;
  const double maximum_rpm = static_cast<double>(std::numeric_limits<int>::max()) / scale;
  const double minimum_rpm = static_cast<double>(std::numeric_limits<int>::min()) / scale;
  if (rpm >= maximum_rpm) {
    return std::numeric_limits<int>::max();
  }
  if (rpm <= minimum_rpm) {
    return std::numeric_limits<int>::min();
  }
  return static_cast<int>(std::round(rpm * scale));
}

uint8_t crc8Maxim(const uint8_t * data, size_t size)
{
  uint8_t crc = 0x00;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 0x01) != 0) {
        crc = static_cast<uint8_t>((crc >> 1) ^ 0x8C);
      } else {
        crc = static_cast<uint8_t>(crc >> 1);
      }
    }
  }
  return crc;
}

namespace
{

void putU16(uint8_t * data, uint16_t value)
{
  data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
  data[1] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t getU16(const uint8_t * data)
{
  return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

int16_t getI16(const uint8_t * data)
{
  return static_cast<int16_t>(getU16(data));
}

HatCommandFrame buildHatCommand(
  uint8_t type, uint16_t sequence, int motor_type, int heartbeat_ms,
  int left_id, int left_cmd, int right_id, int right_cmd, int act, uint8_t flags)
{
  HatCommandFrame frame{};
  frame[0] = HAT_MAGIC_0;
  frame[1] = HAT_MAGIC_1;
  frame[2] = HAT_PROTOCOL_VERSION;
  frame[3] = type;
  putU16(frame.data() + 4, sequence);
  frame[6] = static_cast<uint8_t>(std::clamp(left_id, 0, 255));
  frame[7] = static_cast<uint8_t>(std::clamp(right_id, 0, 255));
  putU16(frame.data() + 8, static_cast<uint16_t>(std::clamp(left_cmd, -32768, 32767)));
  putU16(frame.data() + 10, static_cast<uint16_t>(std::clamp(right_cmd, -32768, 32767)));
  frame[12] = static_cast<uint8_t>(std::clamp(act, 0, 255));
  frame[13] = frame[12];
  frame[14] = static_cast<uint8_t>(std::clamp(motor_type, 0, 255));
  frame[15] = flags;
  const uint16_t encoded_heartbeat = heartbeat_ms < 0 ?
    0xFFFF : static_cast<uint16_t>(std::clamp(heartbeat_ms, 0, 65534));
  putU16(frame.data() + 16, encoded_heartbeat);
  frame[19] = crc8Maxim(frame.data(), frame.size() - 1);
  return frame;
}

MotorFeedback parseHatMotor(const HatFeedbackFrame & frame, size_t offset, int motor_type)
{
  MotorFeedback feedback;
  feedback.id = frame[offset];
  feedback.type = motor_type;
  feedback.action = frame[offset + 2];
  feedback.error = frame[offset + 3];
  feedback.current = getI16(frame.data() + offset + 4);
  feedback.speed = getI16(frame.data() + offset + 6);
  feedback.position = getU16(frame.data() + offset + 8);
  feedback.communication_status = frame[offset + 1];
  feedback.valid = (feedback.communication_status & HAT_MOTOR_VALID) != 0;
  return feedback;
}

}  // namespace

HatCommandFrame buildHatConfigCommand(
  uint16_t sequence, int motor_type, int heartbeat_ms, int left_id, int right_id)
{
  return buildHatCommand(
    HAT_CONFIG_COMMAND, sequence, motor_type, heartbeat_ms,
    left_id, 0, right_id, 0, 2, 0);
}

HatCommandFrame buildHatControlCommand(
  uint16_t sequence, int left_id, int left_cmd, int right_id, int right_cmd,
  int act, bool brake)
{
  return buildHatCommand(
    HAT_CONTROL_COMMAND, sequence, 0, -1,
    left_id, left_cmd, right_id, right_cmd, act, brake ? HAT_FLAG_BRAKE : 0);
}

MotorFeedbackPair parseHatFeedback(
  const HatFeedbackFrame & frame, uint16_t expected_sequence,
  int expected_left_id, int expected_right_id, uint8_t expected_type)
{
  MotorFeedbackPair result;
  if (frame[0] != HAT_MAGIC_0 || frame[1] != HAT_MAGIC_1 ||
    frame[2] != HAT_PROTOCOL_VERSION || frame[3] != expected_type ||
    crc8Maxim(frame.data(), frame.size() - 1) != frame.back())
  {
    return result;
  }

  result.sequence = getU16(frame.data() + 4);
  if (result.sequence != expected_sequence || frame[8] != expected_left_id ||
    frame[18] != expected_right_id)
  {
    return MotorFeedbackPair{};
  }

  result.bridge_status = frame[6];
  result.bridge_error = frame[28];
  result.left = parseHatMotor(frame, 8, frame[7]);
  result.right = parseHatMotor(frame, 18, frame[7]);
  result.valid = true;
  return result;
}

DdsmFrame buildRs485SpeedCommand(int id, int cmd, int act, bool brake)
{
  DdsmFrame frame{};
  const int clamped_id = std::clamp(id, 0, 255);
  const int clamped_cmd = std::clamp(cmd, -32768, 32767);
  const int clamped_act = std::clamp(act, 0, 255);

  frame[0] = static_cast<uint8_t>(clamped_id);
  frame[1] = 0x64;
  frame[2] = static_cast<uint8_t>((clamped_cmd >> 8) & 0xFF);
  frame[3] = static_cast<uint8_t>(clamped_cmd & 0xFF);
  frame[4] = 0x00;
  frame[5] = 0x00;
  frame[6] = static_cast<uint8_t>(clamped_act);
  frame[7] = brake ? 0xFF : 0x00;
  frame[8] = 0x00;
  frame[9] = crc8Maxim(frame.data(), 9);
  return frame;
}

DdsmFrame buildRs485FeedbackRequest(int id)
{
  DdsmFrame frame{};
  frame[0] = static_cast<uint8_t>(std::clamp(id, 0, 255));
  frame[1] = 0x74;
  frame[9] = crc8Maxim(frame.data(), 9);
  return frame;
}

DdsmFrame buildRs485IdQuery()
{
  DdsmFrame frame{};
  frame[0] = 0xC8;
  frame[1] = 0x64;
  frame[9] = crc8Maxim(frame.data(), 9);
  return frame;
}

DdsmFrame buildRs485ModeCommand(int id, int mode)
{
  DdsmFrame frame{};
  frame[0] = static_cast<uint8_t>(std::clamp(id, 0, 255));
  frame[1] = 0xA0;
  frame[9] = static_cast<uint8_t>(std::clamp(mode, 0, 255));
  return frame;
}

MotorFeedback parseRs485MotorFeedback(const DdsmFrame & frame)
{
  MotorFeedback feedback;
  if (crc8Maxim(frame.data(), 9) != frame[9] || frame[0] < 1 || frame[0] > 253 ||
    frame[1] < 1 || frame[1] > 3)
  {
    return feedback;
  }

  const auto to_i16 = [](uint8_t high, uint8_t low) {
      return static_cast<int16_t>((static_cast<uint16_t>(high) << 8) | low);
    };
  const auto to_u16 = [](uint8_t high, uint8_t low) {
      return static_cast<int>((static_cast<uint16_t>(high) << 8) | low);
    };

  feedback.id = frame[0];
  feedback.type = 115;
  feedback.action = frame[1];
  feedback.current = to_i16(frame[2], frame[3]);
  feedback.speed = to_i16(frame[4], frame[5]);
  feedback.position = to_u16(frame[6], frame[7]);
  feedback.error = frame[8];
  feedback.communication_status = HAT_MOTOR_VALID;
  feedback.valid = true;
  return feedback;
}

MotorFeedback parseRs485OtherFeedback(const DdsmFrame & frame)
{
  MotorFeedback feedback = parseRs485MotorFeedback(frame);
  if (!feedback.valid) {
    return feedback;
  }

  feedback.temperature = frame[6];
  feedback.position = frame[7];
  return feedback;
}

}  // namespace ddsm_driver

#ifndef DDSM_DRIVER__DDSM_PROTOCOL_HPP_
#define DDSM_DRIVER__DDSM_PROTOCOL_HPP_

#include <array>
#include <cstddef>
#include <cstdint>

namespace ddsm_driver
{

struct MotorFeedback
{
  int id{0};
  int type{0};
  int speed{0};
  int current{0};
  int action{0};
  int temperature{0};
  int error{0};
  int position{0};
  uint8_t communication_status{0};
  bool valid{false};
};

struct MotorFeedbackPair
{
  MotorFeedback left{};
  MotorFeedback right{};
  uint16_t sequence{0};
  uint8_t bridge_status{0};
  uint8_t bridge_error{0};
  bool valid{false};
};

using DdsmFrame = std::array<uint8_t, 10>;
using HatCommandFrame = std::array<uint8_t, 20>;
using HatFeedbackFrame = std::array<uint8_t, 32>;

constexpr uint8_t HAT_MAGIC_0 = 0xA5;
constexpr uint8_t HAT_MAGIC_1 = 0x5A;
constexpr uint8_t HAT_PROTOCOL_VERSION = 1;
constexpr uint8_t HAT_CONFIG_COMMAND = 0x01;
constexpr uint8_t HAT_CONTROL_COMMAND = 0x02;
constexpr uint8_t HAT_CONFIG_RESPONSE = 0x81;
constexpr uint8_t HAT_CONTROL_RESPONSE = 0x82;
constexpr uint8_t HAT_MOTOR_VALID = 0x01;
constexpr uint8_t HAT_MOTOR_TIMEOUT = 0x02;
constexpr uint8_t HAT_MOTOR_CRC_ERROR = 0x04;
constexpr uint8_t HAT_MOTOR_ID_MISMATCH = 0x08;
constexpr uint8_t HAT_FLAG_BRAKE = 0x01;

int rpmToMotorCommand(int motor_type, double rpm);
uint8_t crc8Maxim(const uint8_t * data, size_t size);
HatCommandFrame buildHatConfigCommand(
  uint16_t sequence, int motor_type, int heartbeat_ms, int left_id, int right_id);
HatCommandFrame buildHatControlCommand(
  uint16_t sequence, int left_id, int left_cmd, int right_id, int right_cmd,
  int act = 3, bool brake = false);
MotorFeedbackPair parseHatFeedback(
  const HatFeedbackFrame & frame, uint16_t expected_sequence,
  int expected_left_id, int expected_right_id, uint8_t expected_type);
DdsmFrame buildRs485SpeedCommand(int id, int cmd, int act = 3, bool brake = false);
DdsmFrame buildRs485FeedbackRequest(int id);
DdsmFrame buildRs485IdQuery();
DdsmFrame buildRs485ModeCommand(int id, int mode);
MotorFeedback parseRs485MotorFeedback(const DdsmFrame & frame);
MotorFeedback parseRs485OtherFeedback(const DdsmFrame & frame);

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__DDSM_PROTOCOL_HPP_

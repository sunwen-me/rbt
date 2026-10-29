#include <iostream>
#include <limits>

#include "ddsm_driver/ddsm_protocol.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

namespace
{
void put_u16(uint8_t * data, uint16_t value)
{
  data[0] = static_cast<uint8_t>(value >> 8);
  data[1] = static_cast<uint8_t>(value & 0xFF);
}

}  // namespace

void test_hat_binary_command_and_feedback()
{
  const HatCommandFrame command = buildHatControlCommand(0x1234, 1, -50, 2, 75, 3);
  DDSM_CHECK(command[0] == HAT_MAGIC_0 && command[1] == HAT_MAGIC_1);
  DDSM_CHECK(command[3] == HAT_CONTROL_COMMAND);
  DDSM_CHECK(command[4] == 0x12 && command[5] == 0x34);
  DDSM_CHECK(command[6] == 1 && command[7] == 2);
  DDSM_CHECK(command[8] == 0xFF && command[9] == 0xCE);
  DDSM_CHECK(command[10] == 0x00 && command[11] == 0x4B);
  DDSM_CHECK(command[19] == crc8Maxim(command.data(), 19));

  HatFeedbackFrame frame{};
  frame[0] = HAT_MAGIC_0;
  frame[1] = HAT_MAGIC_1;
  frame[2] = HAT_PROTOCOL_VERSION;
  frame[3] = HAT_CONTROL_RESPONSE;
  put_u16(frame.data() + 4, 0x1234);
  frame[6] = 0x03;
  frame[7] = 115;
  frame[8] = 1;
  frame[9] = HAT_MOTOR_VALID;
  frame[10] = 2;
  put_u16(frame.data() + 12, static_cast<uint16_t>(-427));
  put_u16(frame.data() + 14, static_cast<uint16_t>(-50));
  put_u16(frame.data() + 16, 4096);
  frame[18] = 2;
  frame[19] = HAT_MOTOR_VALID;
  frame[20] = 2;
  put_u16(frame.data() + 22, 123);
  put_u16(frame.data() + 24, 75);
  put_u16(frame.data() + 26, 8192);
  frame[31] = crc8Maxim(frame.data(), 31);

  const MotorFeedbackPair feedback = parseHatFeedback(
    frame, 0x1234, 1, 2, HAT_CONTROL_RESPONSE);
  DDSM_CHECK(feedback.valid);
  DDSM_CHECK(feedback.left.valid && feedback.right.valid);
  DDSM_CHECK(feedback.left.current == -427);
  DDSM_CHECK(feedback.left.speed == -50);
  DDSM_CHECK(feedback.left.position == 4096);
  DDSM_CHECK(feedback.right.current == 123);
  DDSM_CHECK(feedback.right.speed == 75);

  DDSM_CHECK(!parseHatFeedback(frame, 0x1235, 1, 2, HAT_CONTROL_RESPONSE).valid);

  frame[9] = HAT_MOTOR_TIMEOUT | HAT_MOTOR_CRC_ERROR;
  frame[31] = crc8Maxim(frame.data(), 31);
  const MotorFeedbackPair partial = parseHatFeedback(
    frame, 0x1234, 1, 2, HAT_CONTROL_RESPONSE);
  DDSM_CHECK(partial.valid);
  DDSM_CHECK(!partial.left.valid);
  DDSM_CHECK(partial.left.communication_status ==
    (HAT_MOTOR_TIMEOUT | HAT_MOTOR_CRC_ERROR));

  frame[31] ^= 0x01;
  DDSM_CHECK(!parseHatFeedback(frame, 0x1234, 1, 2, HAT_CONTROL_RESPONSE).valid);
}

void test_ddsm210_command_units()
{
  DDSM_CHECK(rpmToMotorCommand(115, 29.4) == 29);
  DDSM_CHECK(rpmToMotorCommand(210, 29.4) == 294);
  DDSM_CHECK(rpmToMotorCommand(210, -7.5) == -75);
  DDSM_CHECK(rpmToMotorCommand(115, std::numeric_limits<double>::quiet_NaN()) == 0);
  DDSM_CHECK(
    rpmToMotorCommand(115, std::numeric_limits<double>::max()) ==
    std::numeric_limits<int>::max());
  DDSM_CHECK(
    rpmToMotorCommand(210, -std::numeric_limits<double>::max()) ==
    std::numeric_limits<int>::min());
}

void test_rs485_crc_and_command_frames()
{
  const auto speed_cmd = buildRs485SpeedCommand(1, 50, 0);
  DDSM_CHECK(speed_cmd[0] == 0x01);
  DDSM_CHECK(speed_cmd[1] == 0x64);
  DDSM_CHECK(speed_cmd[2] == 0x00);
  DDSM_CHECK(speed_cmd[3] == 0x32);
  DDSM_CHECK(speed_cmd[6] == 0x00);
  DDSM_CHECK(speed_cmd[7] == 0x00);
  DDSM_CHECK(speed_cmd[9] == 0xD3);

  const auto reverse_cmd = buildRs485SpeedCommand(1, -50, 0);
  DDSM_CHECK(reverse_cmd[2] == 0xFF);
  DDSM_CHECK(reverse_cmd[3] == 0xCE);
  DDSM_CHECK(reverse_cmd[9] == 0xDA);

  const auto feedback_request = buildRs485FeedbackRequest(1);
  DDSM_CHECK(feedback_request[0] == 0x01);
  DDSM_CHECK(feedback_request[1] == 0x74);
  DDSM_CHECK(feedback_request[9] == 0x04);

  const auto id_query = buildRs485IdQuery();
  DDSM_CHECK(id_query[0] == 0xC8);
  DDSM_CHECK(id_query[1] == 0x64);
  DDSM_CHECK(id_query[9] == 0xDE);
}

void test_parse_rs485_feedback()
{
  DdsmFrame frame{};
  frame[0] = 1;
  frame[1] = 2;
  frame[2] = 0x01;
  frame[3] = 0xAB;
  frame[4] = 0x00;
  frame[5] = 0x32;
  frame[6] = 0x10;
  frame[7] = 0x00;
  frame[8] = 0x00;
  frame[9] = crc8Maxim(frame.data(), 9);

  const auto feedback = parseRs485MotorFeedback(frame);
  DDSM_CHECK(feedback.valid);
  DDSM_CHECK(feedback.id == 1);
  DDSM_CHECK(feedback.action == 2);
  DDSM_CHECK(feedback.current == 427);
  DDSM_CHECK(feedback.speed == 50);
  DDSM_CHECK(feedback.position == 4096);
  DDSM_CHECK(feedback.error == 0);

  frame[9] ^= 0x01;
  DDSM_CHECK(!parseRs485MotorFeedback(frame).valid);
}

void test_rs485_feedback_semantics()
{
  DdsmFrame other{};
  other[0] = 1;
  other[1] = 2;
  other[4] = 0x00;
  other[5] = 0x32;
  other[6] = 42;
  other[7] = 128;
  other[9] = crc8Maxim(other.data(), 9);

  const auto feedback = parseRs485OtherFeedback(other);
  DDSM_CHECK(feedback.valid);
  DDSM_CHECK(feedback.temperature == 42);
  DDSM_CHECK(feedback.position == 128);

  const auto echo = buildRs485SpeedCommand(1, 50, 0);
  DDSM_CHECK(!parseRs485MotorFeedback(echo).valid);
}

int main()
{
  test_hat_binary_command_and_feedback();
  test_ddsm210_command_units();
  test_rs485_crc_and_command_frames();
  test_parse_rs485_feedback();
  test_rs485_feedback_semantics();
  std::cout << "All ddsm_protocol tests passed\n";
  return 0;
}

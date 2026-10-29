#ifndef DDSM_DRIVER__DDSM_MOTOR_HPP_
#define DDSM_DRIVER__DDSM_MOTOR_HPP_

#include <cstdint>
#include <deque>

#include "ddsm_driver/ddsm_protocol.hpp"
#include "ddsm_driver/serial_comm.hpp"

namespace ddsm_driver
{

class MotorTransport
{
public:
  virtual ~MotorTransport() = default;

  virtual bool configure(int motor_type, int heartbeat_ms, int left_id, int right_id) = 0;
  virtual MotorFeedbackPair exchangePair(
    int left_id, int left_cmd, int right_id, int right_cmd, int act = 3) = 0;
  virtual bool stopPair(int left_id, int right_id) = 0;
};

class HatBinaryMotorTransport : public MotorTransport
{
public:
  explicit HatBinaryMotorTransport(SerialComm & serial);

  bool configure(int motor_type, int heartbeat_ms, int left_id, int right_id) override;
  MotorFeedbackPair exchangePair(
    int left_id, int left_cmd, int right_id, int right_cmd, int act = 3) override;
  bool stopPair(int left_id, int right_id) override;

private:
  MotorFeedbackPair readPair(
    uint16_t sequence, int left_id, int right_id, uint8_t response_type, int timeout_ms);
  uint16_t nextSequence();

  SerialComm & serial_;
  std::deque<uint8_t> rx_buffer_;
  uint16_t sequence_{0};
};

class Rs485MotorTransport : public MotorTransport
{
public:
  explicit Rs485MotorTransport(SerialComm & serial);

  bool configure(int motor_type, int heartbeat_ms, int left_id, int right_id) override;
  MotorFeedbackPair exchangePair(
    int left_id, int left_cmd, int right_id, int right_cmd, int act = 3) override;
  bool stopPair(int left_id, int right_id) override;

private:
  MotorFeedback readFrame(int expected_id, int timeout_ms, bool other_feedback);
  bool setMode(int id, int mode);
  MotorFeedback exchangeSingle(int id, int cmd, int act, bool brake = false);

  SerialComm & serial_;
  std::deque<uint8_t> rx_buffer_;
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__DDSM_MOTOR_HPP_

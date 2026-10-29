#ifndef DDSM_DRIVER__DDSM_FEEDBACK_BUFFER_HPP_
#define DDSM_DRIVER__DDSM_FEEDBACK_BUFFER_HPP_

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "ddsm_driver/ddsm_protocol.hpp"

namespace ddsm_driver
{

class DdsmFeedbackBuffer
{
public:
  void storeFeedback(const MotorFeedback & feedback, int64_t stamp_ns);
  bool getLatest(int motor_id, MotorFeedback & feedback, int64_t & stamp_ns) const;

private:
  struct Entry
  {
    MotorFeedback feedback{};
    int64_t stamp_ns{0};
  };

  mutable std::mutex mutex_;
  std::unordered_map<int, Entry> entries_;
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__DDSM_FEEDBACK_BUFFER_HPP_

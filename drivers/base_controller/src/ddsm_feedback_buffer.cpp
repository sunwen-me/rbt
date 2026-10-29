#include "ddsm_driver/ddsm_feedback_buffer.hpp"

namespace ddsm_driver
{

void DdsmFeedbackBuffer::storeFeedback(const MotorFeedback & feedback, int64_t stamp_ns)
{
  if (!feedback.valid) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  entries_[feedback.id] = Entry{feedback, stamp_ns};
}

bool DdsmFeedbackBuffer::getLatest(int motor_id, MotorFeedback & feedback, int64_t & stamp_ns) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = entries_.find(motor_id);
  if (it == entries_.end()) {
    return false;
  }

  feedback = it->second.feedback;
  stamp_ns = it->second.stamp_ns;
  return true;
}

}  // namespace ddsm_driver

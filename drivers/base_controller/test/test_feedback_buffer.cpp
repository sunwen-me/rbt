#include <iostream>

#include "ddsm_driver/ddsm_feedback_buffer.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

MotorFeedback feedbackFor(int id, int speed, int current)
{
  MotorFeedback feedback;
  feedback.id = id;
  feedback.type = 115;
  feedback.speed = speed;
  feedback.current = current;
  feedback.valid = true;
  return feedback;
}

void test_keeps_latest_feedback_per_motor()
{
  DdsmFeedbackBuffer buffer;

  buffer.storeFeedback(feedbackFor(1, 12, 10), 1000);
  buffer.storeFeedback(feedbackFor(1, 18, 20), 2000);

  MotorFeedback feedback;
  int64_t stamp_ns = 0;
  DDSM_CHECK(buffer.getLatest(1, feedback, stamp_ns));
  DDSM_CHECK(feedback.speed == 18);
  DDSM_CHECK(feedback.current == 20);
  DDSM_CHECK(stamp_ns == 2000);
}

void test_tracks_multiple_motors()
{
  DdsmFeedbackBuffer buffer;
  buffer.storeFeedback(feedbackFor(1, 15, 11), 1000);
  buffer.storeFeedback(feedbackFor(2, 16, 12), 1100);

  MotorFeedback left;
  MotorFeedback right;
  int64_t left_stamp = 0;
  int64_t right_stamp = 0;
  DDSM_CHECK(buffer.getLatest(1, left, left_stamp));
  DDSM_CHECK(buffer.getLatest(2, right, right_stamp));
  DDSM_CHECK(left.speed == 15);
  DDSM_CHECK(right.speed == 16);
}

int main()
{
  test_keeps_latest_feedback_per_motor();
  test_tracks_multiple_motors();
  std::cout << "All feedback buffer tests passed\n";
  return 0;
}

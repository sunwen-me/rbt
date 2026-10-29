#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>

#include "ddsm_driver/native_config.hpp"
#include "ddsm_driver/native_safety.hpp"
#include "test_common.hpp"

namespace
{

rbt::time::ClockIdentity clockIdentity(std::uint8_t first_byte)
{
  std::array<std::uint8_t, 48> bytes{};
  bytes[0] = first_byte;
  auto result = rbt::time::ClockIdentity::create(
    rbt::time::ClockKind::HostMonotonic, rbt::time::ClockSourceId(bytes));
  DDSM_CHECK(result);
  return std::move(result).value();
}

rbt::navwire::VelocityCommand command(
  const rbt::time::ClockIdentity & identity, std::uint64_t sequence,
  std::int64_t stamp_ns, std::int64_t valid_until_ns,
  double linear_x = 0.2, double angular_z = 0.1)
{
  rbt::navwire::VelocityCommand result{};
  result.stamp = rbt::hardware::native::encodeStamp<rbt::navwire::Stamp>(
    {identity, stamp_ns});
  result.source_id = 100;
  result.sequence = sequence;
  result.valid_until_ns = valid_until_ns;
  result.linear_x = linear_x;
  result.angular_z = angular_z;
  return result;
}

rbt::navwire::EmergencyStop estop(
  const rbt::time::ClockIdentity & identity, std::uint64_t sequence,
  std::int64_t stamp_ns, std::uint32_t active)
{
  rbt::navwire::EmergencyStop result{};
  result.stamp = rbt::hardware::native::encodeStamp<rbt::navwire::Stamp>(
    {identity, stamp_ns});
  result.source_id = 2;
  result.sequence = sequence;
  result.active = active;
  return result;
}

void testCommandValidationAndExpiry()
{
  constexpr std::int64_t now_ns = 1'000'000'000;
  const auto identity = clockIdentity(0x42);
  const auto other_identity = clockIdentity(0x24);
  const rbt::time::Timestamp now{identity, now_ns};
  ddsm_driver::NativeCommandGate gate;

  auto wrong_source = command(identity, 50, now_ns, now_ns + 100'000'000);
  wrong_source.source_id = 101;
  DDSM_CHECK(gate.accept(wrong_source, now) == ddsm_driver::CommandRejection::SOURCE);
  DDSM_CHECK(gate.commandSequence() == 0);

  auto wrong_clock = command(other_identity, 50, now_ns, now_ns + 100'000'000);
  DDSM_CHECK(gate.accept(wrong_clock, now) == ddsm_driver::CommandRejection::CLOCK);
  DDSM_CHECK(gate.commandSequence() == 0);

  auto future_stamp = command(identity, 50, now_ns + 1, now_ns + 100'000'000);
  DDSM_CHECK(gate.accept(future_stamp, now) == ddsm_driver::CommandRejection::TIME);
  auto stale_stamp = command(identity, 50, now_ns - 601'000'000, now_ns + 100'000'000);
  DDSM_CHECK(gate.accept(stale_stamp, now) == ddsm_driver::CommandRejection::TIME);
  DDSM_CHECK(gate.commandSequence() == 0);

  auto oversized_ttl = command(identity, 50, now_ns, now_ns + 300'000'000);
  DDSM_CHECK(gate.accept(oversized_ttl, now) == ddsm_driver::CommandRejection::TTL);
  DDSM_CHECK(gate.commandSequence() == 0);

  auto accepted = command(identity, 1, now_ns, now_ns + 100'000'000);
  DDSM_CHECK(gate.accept(accepted, now) == ddsm_driver::CommandRejection::NONE);
  const auto running = gate.tick(now_ns + 50'000'000);
  DDSM_CHECK(running.linear_x == 0.2);
  DDSM_CHECK(!running.watchdog_expired);

  const auto expired = gate.tick(now_ns + 100'000'000);
  DDSM_CHECK(expired.linear_x == 0.0 && expired.angular_z == 0.0);
  DDSM_CHECK(expired.watchdog_expired);

  constexpr auto max_ns = std::numeric_limits<std::int64_t>::max();
  const rbt::time::Timestamp near_limit{identity, max_ns - 100};
  ddsm_driver::NativeCommandGate overflow_gate;
  DDSM_CHECK(overflow_gate.accept(
    command(identity, 1, max_ns - 100, max_ns), near_limit) ==
    ddsm_driver::CommandRejection::NONE);
  DDSM_CHECK(overflow_gate.tick(max_ns - 1).linear_x == 0.2);
  DDSM_CHECK(overflow_gate.tick(max_ns).watchdog_expired);
}

void testEstopFreshRearm()
{
  constexpr std::int64_t now_ns = 2'000'000'000;
  const auto identity = clockIdentity(0x42);
  const rbt::time::Timestamp now{identity, now_ns};
  ddsm_driver::NativeCommandGate gate;
  DDSM_CHECK(gate.accept(
    command(identity, 1, now_ns, now_ns + 100'000'000), now) ==
    ddsm_driver::CommandRejection::NONE);

  // Routine inactive heartbeats must not erase a live command.
  DDSM_CHECK(gate.accept(estop(identity, 1, now_ns, 0), now) ==
    ddsm_driver::CommandRejection::NONE);
  DDSM_CHECK(gate.tick(now_ns).linear_x == 0.2);

  DDSM_CHECK(gate.accept(estop(identity, 2, now_ns, 1), now) ==
    ddsm_driver::CommandRejection::NONE);
  DDSM_CHECK(gate.emergencyStop());
  DDSM_CHECK(gate.tick(now_ns).emergency_stop);

  DDSM_CHECK(gate.accept(estop(identity, 3, now_ns - 700'000'000, 0), now) ==
    ddsm_driver::CommandRejection::TIME);
  DDSM_CHECK(gate.emergencyStop());
  DDSM_CHECK(gate.accept(estop(identity, 3, now_ns, 0), now) ==
    ddsm_driver::CommandRejection::NONE);
  DDSM_CHECK(!gate.emergencyStop());
  DDSM_CHECK(gate.tick(now_ns).watchdog_expired);

  DDSM_CHECK(gate.accept(
    command(identity, 2, now_ns, now_ns + 100'000'000), now) ==
    ddsm_driver::CommandRejection::NONE);
  DDSM_CHECK(gate.tick(now_ns).linear_x == 0.2);
}

void testIoFaultLatchAndRecovery()
{
  constexpr std::int64_t now_ns = 3'000'000'000;
  const auto identity = clockIdentity(0x42);
  const rbt::time::Timestamp now{identity, now_ns};
  ddsm_driver::NativeCommandGate gate;
  ddsm_driver::IoFaultLatch latch(5, 5);
  DDSM_CHECK(gate.accept(
    command(identity, 1, now_ns, now_ns + 100'000'000), now) ==
    ddsm_driver::CommandRejection::NONE);

  for (int cycle = 0; cycle < 4; ++cycle) {
    DDSM_CHECK(!latch.observe(false, false));
  }
  DDSM_CHECK(latch.observe(false, false));
  DDSM_CHECK(latch.active());
  gate.clearCommand();
  DDSM_CHECK(gate.tick(now_ns).watchdog_expired);

  for (int cycle = 0; cycle < 4; ++cycle) {
    DDSM_CHECK(!latch.observe(true, false));
  }
  DDSM_CHECK(latch.observe(true, false));
  DDSM_CHECK(!latch.active());
  DDSM_CHECK(gate.tick(now_ns).watchdog_expired);
  DDSM_CHECK(gate.accept(
    command(identity, 2, now_ns, now_ns + 100'000'000), now) ==
    ddsm_driver::CommandRejection::NONE);

  ddsm_driver::IoFaultLatch motor_error_latch(5, 5);
  DDSM_CHECK(motor_error_latch.observe(false, true));
  DDSM_CHECK(motor_error_latch.active());
}

void testCurrentYamlValues()
{
  ddsm_driver::NativeOptions options;
  options.config_file = RBT_BASE_CONTROLLER_SOURCE_CONFIG;
  const auto config = ddsm_driver::loadDriverConfig(options);
  DDSM_CHECK(config.wheel_radius == 0.05);
  DDSM_CHECK(config.wheel_separation == 0.395);
  DDSM_CHECK(config.control_rate == 100.0);
  DDSM_CHECK(config.command_timeout == 0.6);
  DDSM_CHECK(config.feedback_timeout == 0.05);
  DDSM_CHECK(config.left_motor_sign == 1);
  DDSM_CHECK(config.right_motor_sign == -1);
  DDSM_CHECK(config.protocol == "hat_binary");
  DDSM_CHECK(config.max_motor_cmd == 330);
  DDSM_CHECK(config.imu_topic == "/imu/data");
}

}  // namespace

int main()
{
  testCommandValidationAndExpiry();
  testEstopFreshRearm();
  testIoFaultLatchAndRecovery();
  testCurrentYamlValues();
  std::cout << "native safety tests passed\n";
  return 0;
}

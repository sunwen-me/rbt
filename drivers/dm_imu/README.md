# Native DM IMU driver

The native ARM64 driver is deployed on the AidLux robot and its real serial-to-IPC
stream has been verified. See [deployment and service commands](DEPLOYMENT.md)
for the exact installation, results, and remaining calibration scope.

`rbt_dm_imu` reads the DM IMU's three consecutive 19-byte records and publishes
the newest validated sample as `rbt::hardware::Imu` at a configurable rate
(default 200 Hz). It has no ROS dependency. The output uses SI units, a host monotonic receive timestamp, and
zero covariance because the wire protocol supplies no uncertainty values.
The sequence starts from host monotonic time and increments per sample, so a
same-host process restart can continue above recent sequence numbers while a
consumer stays running. Sequence numbers count published IPC messages; raw
samples intentionally skipped by the rate setting do not create sequence gaps.
Each message timestamp is still taken after its full frame passes validation.

Build from the repository root with CMake and the installed RBT SDK. The
default settings are also in `config/dm_imu.yaml`; configuration is optional:

```sh
rbt_dm_imu --config drivers/dm_imu/config/dm_imu.yaml
```

`publish_rate_hz` in YAML sets the publication target from 1 to 1000 Hz. The
driver drains and validates the existing serial stream, publishes the newest
valid sample at a due publication slot, skips missed slots without catch-up
bursts, and never republishes an old sample during an input outage. This setting
does not send a device-rate command or save device parameters.

`--serial-port`, `--topic`, `--duration SEC`, `--gyro-unit rad_s|deg_s`, and
`--accel-unit m_s2|g` override file values. `--configure-device` sends the
reference driver's enter-setting, accel, gyro, Euler, quaternion-off, and
1000 Hz commands, then exits setting mode. It does not save parameters.
`--save-parameters` opts into the reference save command and also enables
startup configuration.

The read-only `rbt_dm_imu_probe` subscribes to the native `rbt.hardware::Imu`
stream without opening the serial device or publishing commands. It defaults to
a fixed 1 ms polling period and observes `/imu/data` for five seconds. The
optional `--mode event` uses sample readiness and retries transient subscriber
startup failures. Both modes stop on their duration or SIGINT/SIGTERM and print
one JSON summary:

```sh
rbt_dm_imu_probe --topic /imu/data --duration 5
```

ARM64 CPU, reception, and latency measurements are recorded in the
[optimization report](../../output/dm-imu-optimization/20261028T180357Z/REPORT.md).
Polling remains the default; event mode is opt-in because the 1 kHz experiment
used more CPU despite lower receive latency.

The probe checks finite quaternion and vector values, a quaternion norm within
0.05 of one, a nonempty null-terminated frame, a matching fresh host monotonic
stamp (at most 500 ms old), and nonzero increasing sequence numbers. Its rate
uses the interval between the first and last received samples. Exit status zero
requires at least five valid samples, no invalid samples, and a latest stamp
within 500 ms at exit. A successful run confirms software stream reception and
message consistency; physical sensor units, axes, mounting, and calibration
still require hardware-specific validation. Building and installing the probe
alone do not establish that the sensor is publishing.

The first record begins with the established bytes `55 aa 01 01`. The next two
record tags and all three final bytes are unknown from the available reference,
so they are not guessed or checked by default. Set `protocol.expected_tags` or
`protocol.expected_terminators` after confirming values from a hardware capture.
Each record checks its first 16 bytes against the 16-bit CRC stored in
little-endian order. Starting from `0xffff`, the device recurrence computes a
high-byte table contribution with polynomial `0x1021`, then updates the running
CRC as `((crc << 1) ^ contribution) & 0xffff` once per input byte. This differs
from the common CRC-CCITT byte update, which shifts the running CRC eight times
per byte. All three CRCs and all nine finite float values are required before a
sample is published. The parser drops bytes until it finds the known first-record
anchor, then checks exactly the following 57-byte group.

The default source-unit assumptions are rad/s and m/s², matching the old
pass-through behavior; the protocol evidence does not establish those units.
Euler wire values are converted from degrees to a normalized quaternion once.
Sensor axes are left as reported by the device; mounting extrinsics belong in
the robot's external transform configuration.

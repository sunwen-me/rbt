# Linux V4L2 camera with stamped bytes

This example captures real frames from an explicitly selected Linux V4L2
device and publishes them as `rbt.time/StampedBytes@1`. It has no synthetic
camera fallback. The producer requests YUYV at 640x480, reports the driver’s
actual negotiated format, and fails if the device cannot stream single-plane
YUYV video.

Build both programs from the repository root using the maintained C++ SDK
prefix:

```sh
./scripts/bootstrap_iceoryx2_cpp.sh
cmake -S sdk/cpp -B target/rbt-time-camera-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/target/iox2-rbt-0.10.0-cpp-install"
cmake --build target/rbt-time-camera-build --target rbt_time_camera rbt_time_camera_consumer
```

Run a 300-second physical capture and consumer on the selected camera with a
stream stop/close/reopen after 120 seconds:

```sh
service="rbt_time_camera_$$"
target/rbt-time-camera-build/rbt_time_camera \
  --device /dev/video0 --service "$service" --duration-sec 300 \
  --restart-after-sec 120 >producer.jsonl 2>producer.stderr &
producer_pid=$!
# Subscribers open the existing publisher-owned readiness service.
ready=0
for attempt in {1..100}; do
  if rg -q '\"event\":\"device\"' producer.jsonl; then ready=1; break; fi
  kill -0 "$producer_pid" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" -ne 1 ]; then
  kill "$producer_pid" 2>/dev/null || true
  wait "$producer_pid" || true
  cat producer.stderr >&2
  exit 1
fi
target/rbt-time-camera-build/rbt_time_camera_consumer \
  --service "$service" --duration-sec 300 >consumer.jsonl 2>consumer.stderr &
consumer_pid=$!
wait "$producer_pid"
producer_status=$?
wait "$consumer_pid"
consumer_status=$?
test "$producer_status" -eq 0 -a "$consumer_status" -eq 0
```

The producer emits JSON Lines with `VIDIOC_QUERYCAP`, actual width, height,
stride, FourCC, sizeimage, frame interval when available, and every dequeued
buffer’s raw timestamp, raw V4L2 flags, source, receive stamp, and device
session generation. Its application bytes are a 48-byte `RBTVCAM1` metadata
record followed by the mapped frame bytes. The metadata keeps the raw driver
timestamp and flags even when those values cannot be used as a capture time.
Copying a mapped camera frame into the native shared-memory loan is the one
required DMA ingress copy; the transport sample is then borrowed directly.

The capture stamp is present only when V4L2 marks the buffer timestamp as
`MONOTONIC`, the source as start-of-exposure, and the time API verifies a zero
monotonic time-namespace offset. V4L2 end-of-frame timestamps remain driver
event times and leave capture absent. Unknown, realtime, copied, or otherwise
unverified driver timestamps are preserved as raw metadata and are never
converted to system time. `driver_to_receive_ns` is reported separately only
when the driver timestamp and host receive timestamp have a proven common
monotonic basis. Negative latency samples are rejected and counted, and the
mean reports a status instead of overflowing its bounded accumulator. A
malformed or unrepresentable V4L2 timeval fails before publication and logs
its raw seconds, microseconds, sequence, and flags. Host receive uncertainty
remains unknown (`null`).

`--restart-after-sec` exercises `STREAMOFF`, unmapping/releasing buffers,
closing the descriptor, and reopening the same device. The app increments
only its driver-session generation and marks the previous device association
invalid. This does not simulate a physical USB unplug/replug and does not claim
that the host monotonic clock reset.

A repeated or backwards V4L2 sequence also ends the driver sequence
association. The example records `driver_sequence_discontinuity` with both raw
sequence values and advances the session generation before publishing that
frame. Its cause remains unconfirmed; it is not labelled a physical unplug or
a host-clock reset. Normal u32 sequence wrap remains in the same generation.

The consumer validates the timing prefix, host clock source, capture/V4L2
source consistency, app metadata and YUYV dimensions, checks sequence gaps,
and checks the borrowed payload through a const span. Each `frame_checked`
line includes an FNV-1a 64-bit checksum of the raw pixels so separate language
consumers can compare the same sequence. The final summary reports capture
known/unknown counts, actual steady-clock elapsed milliseconds, dequeued and
published counts, driver sequence gaps/duplicates/out-of-order frames, forward
and backwards timestamp steps, valid/negative/arithmetic-invalid latency
counts, separate driver-event and capture-to-receive min/max/mean statistics,
session generations, and stream resource counters before and after cleanup.
Capture-to-receive values appear only for comparable SOE samples. Each latency
mean is `null` with an explicit status if it has no valid samples or its
accumulator overflows.

Running the producer changes the camera’s negotiated format and begins
physical capture. It uses only the explicitly selected `--device` argument;
if that device cannot open or stream, it exits with the driver error.

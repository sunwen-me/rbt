# Physical camera with capture-time TF

This example joins the existing V4L2 `CameraComponent` to a native
`StampedBytes` stream and resolves the camera's static extrinsic at the time
reported by the camera driver. The camera service is created from a fresh
physical capability identity for each activation. The calibration component
serves a complete static snapshot as well as update notifications, so a
perception component that joins later can recover the current transform.

The C++ camera keeps the v0.7 `RBTVCAM1` 48-byte application prefix whenever
`--frame-id` is omitted. Passing `--frame-id camera_optical` opts into
`RBTVCAM2`: the same first 48 metadata bytes, followed by the canonical
64-byte generated TF frame identity at offset 48 and pixel bytes at offset
112. The `StampedBytes` 192-byte timing prefix and native shared-memory loan
are unchanged.

## Software-only check

```sh
scripts/test_tf_hardware.py --self-test softwareonly
```

This only checks an in-memory format and timing fixture. Its result is labeled
`SOFTWARE_FIXTURE`; it does not open a device or claim a physical-camera pass.

## Build and hardware acceptance

Choose a V4L2 character device and a reviewed static extrinsics file
explicitly. The config must contain a static path from `base_link` to the
selected frame. The example `docs/tf-config-example.toml` is software fixture
data, not measured calibration. No device discovery or selection is done by
the gate.

```sh
export RBT_PHASE4_TOOLCHAIN="$PWD/target/phase4-toolchain"
export RBT_IOX2_PREFIX="$PWD/target/iox2-rbt-0.10.0-cpp-install"
export PATH="$RBT_PHASE4_TOOLCHAIN/bin:$PATH"
export CC="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-g++"
export AR="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
export LD_LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_IOX2_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_IOX2_PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
cargo build -p composer -p rbt-tf-camera
cmake -S sdk/cpp -B target/tf-camera-cpp-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH="$RBT_IOX2_PREFIX"
cmake --build target/tf-camera-cpp-build --target rbt_time_camera
scripts/test_tf_hardware.py \
  --device /dev/video2 \
  --config /absolute/path/to/camera-extrinsics.toml \
  --frame-id camera_optical \
  --duration 300
```

The gate builds the Rust components and existing C++ camera target using the
pinned toolchain and maintained native prefix. It starts Composer, the
camera, and calibration; waits five seconds; then starts perception for at
least 300 seconds. It saves the exact commands, component JSONL, resource
samples, and a summary under `target/tf-camera-hardware-logs/`. A successful
hardware report requires matching camera/perception checksums, the same
capture timestamp and frame identity, a revision-1 static source after the
late join, successful lookup at the actual capture time, zero retained native
borrows, bounded process resources, and clean process-group teardown.

## Manual component launch

Build first with the commands used by the gate, then start each process using
the same Composer address. Keep the selected device and config paths explicit.

```sh
export RBT_COMPOSER_ADDR=127.0.0.1:7447
target/debug/composer
```

In separate terminals, start the camera and calibration components. After both
emit their ready records, wait five seconds before starting perception in a
third terminal:

```sh
target/debug/rbt-tf-camera camera \
  --device /dev/video2 \
  --camera-exe "$PWD/target/tf-camera-cpp-build/rbt_time_camera" \
  --frame-id camera_optical \
  --duration-sec 360
```

```sh
target/debug/rbt-tf-camera calibration \
  --config /absolute/path/to/camera-extrinsics.toml
```

```sh
sleep 5
target/debug/rbt-tf-camera perception \
  --frame-id camera_optical \
  --duration-sec 300
```

The printed calibration message is `Configured extrinsic — calibration
accuracy unverified`. This example makes no claim that a supplied transform
was measured or validated against the physical camera. Queries to `odom` and
`map` should report missing transforms. Physical dynamic TF remains `NOT RUN`.

from __future__ import annotations

import contextlib
import dataclasses
import os
import re
import shlex
import subprocess
import threading
import time
from pathlib import Path

import numpy as np
import pytest

import rbt

from conftest import REPO_ROOT, unique_service


IMAGE_WIDTH = 1_920
IMAGE_HEIGHT = 1_080
IMAGE_CHANNELS = 3
IMAGE_STRIDE = IMAGE_WIDTH * IMAGE_CHANNELS
IMAGE_FRAMES = 10
IMAGE_LAYOUT_HASH = "943fa4008cf0f55f89a281b051c69a6f"
POINT_COUNTS = (0, 1, 100, 100_000)
POINT_DTYPE = rbt.POINT_XYZI_DTYPE


def _native_binary(variable: str, default: Path) -> Path:
    configured = os.environ.get(variable)
    path = Path(configured) if configured else default
    if not path.is_file():
        pytest.skip(f"native interop binary is unavailable: {path}")
    return path


def _rust_binary() -> Path:
    return _native_binary(
        "RBT_PHASE4_INTEROP", REPO_ROOT / "target" / "release" / "phase4-interop"
    )


def _cpp_binary() -> Path:
    return _native_binary(
        "RBT_CPP_INTEROP", REPO_ROOT / "target" / "rbt-cpp-build" / "interop_cpp"
    )


def _image_descriptor_with_resources(
    service: str, max_loans: int, max_borrowed: int
) -> rbt.StreamDescriptor:
    return dataclasses.replace(
        rbt.StreamDescriptor.image(service),
        resources=rbt.StreamResourceConfig(
            max_loans=max_loans, max_borrowed=max_borrowed
        ),
    )


class _NativeChild:
    """Run one native consumer while retaining all output for assertion failures."""

    def __init__(self, arguments: tuple[object, ...], ready_pattern: str) -> None:
        self.arguments = tuple(str(argument) for argument in arguments)
        self._ready_pattern = re.compile(ready_pattern)
        self._lines: list[str] = []
        self._lines_lock = threading.Lock()
        self._ready = threading.Event()
        self.process = subprocess.Popen(
            list(self.arguments),
            cwd="/tmp",
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        assert self.process.stdout is not None
        self._reader = threading.Thread(
            target=self._read_output,
            args=(self.process.stdout,),
            daemon=True,
        )
        self._reader.start()

    def _read_output(self, stream: object) -> None:
        # Popen's text stdout is line-iterable. Keeping the reader separate
        # lets wait_ready have a real deadline without leaving a blocking
        # readline in the test thread.
        for line in stream:  # type: ignore[union-attr]
            with self._lines_lock:
                self._lines.append(line)
            if self._ready_pattern.search(line):
                self._ready.set()

    @property
    def output(self) -> str:
        with self._lines_lock:
            return "".join(self._lines)

    def wait_ready(self, timeout: float = 15.0) -> str:
        if not self._ready.wait(timeout):
            self.close()
            raise AssertionError(
                "native consumer did not reach a stable READY line "
                f"({shlex.join(self.arguments)}):\n{self.output}"
            )
        # A READY line is emitted only after the native endpoint is opened,
        # but a short stability check catches a consumer that immediately
        # failed while making its endpoint visible.
        time.sleep(0.05)
        return_code = self.process.poll()
        if return_code is not None:
            raise AssertionError(
                f"native consumer exited after READY ({return_code}; "
                f"{shlex.join(self.arguments)}):\n{self.output}"
            )
        return self.output

    def finish(self, timeout: float = 20.0) -> str:
        try:
            return_code = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.close()
            raise AssertionError(
                f"native consumer timed out ({shlex.join(self.arguments)}):\n{self.output}"
            ) from None
        self._reader.join(timeout=2.0)
        output = self.output
        if return_code != 0:
            raise AssertionError(
                f"native consumer failed ({return_code}; {shlex.join(self.arguments)}):\n{output}"
            )
        return output

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=3.0)
        self._reader.join(timeout=2.0)

    def __enter__(self) -> _NativeChild:
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()


def _fill_byte_pattern(payload: np.ndarray, sequence: int) -> None:
    """Fill a 1-D uint8 view without allocating a payload-sized temporary."""
    assert payload.dtype == np.uint8
    for offset in range(256):
        payload[offset::256] = (sequence + offset) & 0xFF


def _publish_image_frames(
    publisher: rbt.ImagePublisher,
    width: int,
    height: int,
    stride: int,
    frames: int,
    *,
    period: float = 0.05,
    validate_padding: bool = False,
) -> int:
    sent = 0
    row_bytes = width * IMAGE_CHANNELS
    for sequence in range(frames):
        loan = publisher.loan(
            width,
            height,
            rbt.PixelFormat.RGB8,
            stride=stride,
            sequence=sequence,
            timestamp_ns=sequence * 1_000_000,
        )
        image = loan.numpy()
        assert image.shape == (height, width, IMAGE_CHANNELS)
        assert image.strides == (stride, IMAGE_CHANNELS, 1)
        assert image.dtype == np.uint8
        assert image.flags.writeable
        assert not image.flags.owndata
        assert image.__array_interface__["data"][0] == loan._debug_payload_address()

        if sequence == 0:
            with pytest.raises(rbt.LoanStillMappedError):
                loan.send()

        if validate_padding:
            raw_owner = loan.payload_view()
            raw = np.frombuffer(raw_owner, dtype=np.uint8, count=loan.payload_nbytes)
            assert raw.shape == (stride * height,)
            assert raw.__array_interface__["data"][0] == loan._debug_payload_address()
            _fill_byte_pattern(raw, sequence)
            for row in range(height):
                padding_start = row * stride + row_bytes
                padding_end = (row + 1) * stride
                for index in range(padding_start, padding_end):
                    assert int(raw[index]) == (sequence + index) % 256
            del raw, raw_owner, image
        else:
            flat = image.reshape(-1)
            _fill_byte_pattern(flat, sequence)
            del flat, image

        assert loan._debug_export_count() == 0
        loan.send()
        sent += 1
        if sequence + 1 < frames:
            time.sleep(period)
    return sent


def _fill_points(points: np.ndarray) -> None:
    """Fill PointXYZI scalars in place; no full payload temporary is made."""
    x = points["x"]
    y = points["y"]
    z = points["z"]
    intensity = points["intensity"]
    for index in range(points.shape[0]):
        value = float(index)
        x[index] = value
        y[index] = value * 2.0
        z[index] = -value
        intensity[index] = float(index % 31)


def _publish_pointcloud_frames(publisher: rbt.PointCloudPublisher) -> int:
    sent = 0
    for sequence, count in enumerate(POINT_COUNTS):
        loan = publisher.loan(
            count,
            sequence=sequence,
            timestamp_ns=sequence * 1_000_000,
            dense=True,
        )
        points = loan.numpy()
        assert points.shape == (count,)
        assert points.dtype == POINT_DTYPE
        assert points.flags.writeable
        assert not points.flags.owndata
        assert points.__array_interface__["data"][0] == loan._debug_payload_address()
        assert loan.payload_nbytes == count * POINT_DTYPE.itemsize
        _fill_points(points)
        del points
        assert loan._debug_export_count() == 0
        loan.send()
        sent += 1
        if sequence + 1 < len(POINT_COUNTS):
            # The native subscribers use a sensor queue of depth one. This
            # gives their WaitSet handler time to release each large sample.
            time.sleep(0.20)
    return sent


def test_python_image_publisher_to_rust_zero_copy() -> None:
    service = unique_service("publisher-rust-image")
    publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
    with _NativeChild(
        (
            _rust_binary(),
            "image-subscriber",
            service,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            IMAGE_FRAMES,
        ),
        r"^IMAGE_SUBSCRIBER ready service=",
    ) as child:
        child.wait_ready()
        assert _publish_image_frames(
            publisher,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            IMAGE_FRAMES,
        ) == IMAGE_FRAMES
        output = child.finish()
    assert re.search(r"IMAGE_SUBSCRIBER complete .*frames=10\b", output), output


def test_python_image_publisher_to_cpp_zero_copy() -> None:
    service = unique_service("publisher-cpp-image")
    publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
    with _NativeChild(
        (_cpp_binary(), "image-subscriber", service, IMAGE_FRAMES, 0),
        r"^IMAGE_CONSUMER ready service=",
    ) as child:
        child.wait_ready()
        assert _publish_image_frames(
            publisher,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            IMAGE_FRAMES,
        ) == IMAGE_FRAMES
        output = child.finish()
    assert re.search(r"IMAGE_CONSUMER complete .*frames=10\b", output), output


def test_python_padded_image_publisher_to_rust_uses_raw_payload_view() -> None:
    service = unique_service("publisher-rust-padded-image")
    width, height, stride = 640, 480, 2_048
    publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
    with _NativeChild(
        (_rust_binary(), "image-subscriber", service, width, height, stride, 1),
        r"^IMAGE_SUBSCRIBER ready service=",
    ) as child:
        child.wait_ready()
        assert _publish_image_frames(
            publisher,
            width,
            height,
            stride,
            1,
            validate_padding=True,
        ) == 1
        output = child.finish()
    assert re.search(r"IMAGE_SUBSCRIBER complete .*frames=1\b", output), output


def test_rust_image_publisher_to_cpp_uses_custom_resources() -> None:
    service = unique_service("publisher-rust-custom-image")
    with _NativeChild(
        (_rust_binary(), "image-publisher", service, 4, 2),
        r"^IMAGE_PUBLISHER ready service=",
    ) as publisher:
        publisher.wait_ready()
        with _NativeChild(
            (_cpp_binary(), "image-subscriber", service, 3, 0, 4, 2),
            r"^IMAGE_CONSUMER ready service=",
        ) as subscriber:
            subscriber.wait_ready()
            publisher_output = publisher.finish()
            subscriber_output = subscriber.finish()

    assert re.search(
        rf"IMAGE_PUBLISHER ready .*hash={IMAGE_LAYOUT_HASH} .*max_loans=4 max_borrowed=2",
        publisher_output,
    ), publisher_output
    assert re.search(
        rf"IMAGE_PUBLISHER complete .*frames=3 .*hash={IMAGE_LAYOUT_HASH}",
        publisher_output,
    ), publisher_output
    assert re.search(
        rf"IMAGE_CONSUMER bound .*layout={IMAGE_LAYOUT_HASH}", subscriber_output
    ), subscriber_output
    assert re.search(
        r"IMAGE_CONSUMER ready .*max_loans=4 max_borrowed=2", subscriber_output
    ), subscriber_output
    assert re.search(r"IMAGE_CONSUMER complete .*frames=3\b", subscriber_output), subscriber_output


def test_cpp_image_publisher_to_python_uses_matching_custom_resources() -> None:
    service = unique_service("publisher-cpp-custom-image")
    with _NativeChild(
        (_cpp_binary(), "image-publisher", service, 1, 3, 2),
        r"^IMAGE_PUBLISHER ready service=",
    ) as publisher:
        publisher.wait_ready()
        descriptor = _image_descriptor_with_resources(service, 3, 2)
        subscriber = rbt.ImageSubscriber.open(descriptor, max_borrowed=2)
        assert subscriber.resources == rbt.StreamResourceConfig(
            max_loans=3, max_borrowed=2
        )
        assert subscriber.descriptor.message_type.layout_hash == IMAGE_LAYOUT_HASH

        sample = subscriber.receive(timeout=15.0)
        assert sample is not None
        image = sample.numpy()
        assert image.shape == (IMAGE_HEIGHT, IMAGE_WIDTH, IMAGE_CHANNELS)
        assert image.strides == (IMAGE_STRIDE, IMAGE_CHANNELS, 1)
        assert image.dtype == np.uint8
        assert sample.payload_nbytes == IMAGE_STRIDE * IMAGE_HEIGHT
        assert int(image[0, 0, 0]) == sample.sequence % 256
        assert int(image[-1, -1, -1]) == (
            sample.sequence + sample.payload_nbytes - 1
        ) % 256
        del image, sample
        publisher_output = publisher.finish()

    assert re.search(
        rf"IMAGE_PUBLISHER ready .*hash={IMAGE_LAYOUT_HASH} .*max_loans=3 max_borrowed=2",
        publisher_output,
    ), publisher_output
    assert re.search(r"IMAGE_PUBLISHER complete .*frames=40\b", publisher_output), publisher_output


def test_python_image_publisher_to_rust_uses_custom_resources() -> None:
    service = unique_service("publisher-python-custom-image")
    publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(service), max_loans=4, max_borrowed=1
    )
    assert publisher.resources == rbt.StreamResourceConfig(max_loans=4, max_borrowed=1)
    assert publisher.descriptor.message_type.layout_hash == IMAGE_LAYOUT_HASH
    with _NativeChild(
        (
            _rust_binary(),
            "image-subscriber",
            service,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            3,
            4,
            1,
        ),
        r"^IMAGE_SUBSCRIBER ready service=",
    ) as child:
        child.wait_ready()
        assert _publish_image_frames(
            publisher,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            3,
        ) == 3
        output = child.finish()

    assert re.search(
        rf"IMAGE_SUBSCRIBER ready .*hash={IMAGE_LAYOUT_HASH} .*max_loans=4 max_borrowed=1",
        output,
    ), output
    assert re.search(
        rf"IMAGE_SUBSCRIBER complete .*frames=3 .*hash={IMAGE_LAYOUT_HASH}", output
    ), output


def _run_pointcloud_interop(binary: Path, command: str) -> str:
    service = unique_service(f"publisher-{command}-pointcloud")
    publisher = rbt.PointCloudPublisher.open(rbt.StreamDescriptor.point_cloud(service))
    with _NativeChild(
        (binary, command, service),
        r"^POINTCLOUD_SUBSCRIBER ready service=",
    ) as child:
        child.wait_ready()
        assert _publish_pointcloud_frames(publisher) == len(POINT_COUNTS)
        return child.finish(timeout=30.0)


def test_python_pointcloud_publisher_to_rust_zero_copy() -> None:
    output = _run_pointcloud_interop(_rust_binary(), "pointcloud-subscriber")
    assert re.search(r"POINTCLOUD_SUBSCRIBER complete .*frames=4\b", output), output


def test_python_pointcloud_publisher_to_cpp_zero_copy() -> None:
    output = _run_pointcloud_interop(_cpp_binary(), "pointcloud-subscriber")
    assert re.search(r"POINTCLOUD_SUBSCRIBER complete .*frames=4\b", output), output


@pytest.mark.full
def test_python_sensor_publisher_handles_fast_slow_and_stalled_consumers() -> None:
    service = unique_service("publisher-sensor-pressure")
    frames = 40
    publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
    started = time.monotonic()
    with contextlib.ExitStack() as children:
        rust_fast = children.enter_context(
            _NativeChild(
                (
                    _rust_binary(),
                    "image-subscriber",
                    service,
                    IMAGE_WIDTH,
                    IMAGE_HEIGHT,
                    IMAGE_STRIDE,
                    frames,
                ),
                r"^IMAGE_SUBSCRIBER ready service=",
            )
        )
        cpp_slow = children.enter_context(
            _NativeChild(
                (_cpp_binary(), "image-subscriber", service, 3, 200),
                r"^IMAGE_CONSUMER ready service=",
            )
        )
        cpp_stalled = children.enter_context(
            _NativeChild(
                (_cpp_binary(), "image-subscriber-hold", service, 3_000),
                r"^IMAGE_CONSUMER_HOLD ready service=",
            )
        )
        for child in (rust_fast, cpp_slow, cpp_stalled):
            child.wait_ready()

        assert _publish_image_frames(
            publisher,
            IMAGE_WIDTH,
            IMAGE_HEIGHT,
            IMAGE_STRIDE,
            frames,
            period=1.0 / 30.0,
        ) == frames
        elapsed = time.monotonic() - started
        rust_output = rust_fast.finish(timeout=20.0)
        slow_output = cpp_slow.finish(timeout=20.0)
        stalled_output = cpp_stalled.finish(timeout=6.0)

    assert elapsed < 8.0, f"stalled peer blocked publisher for {elapsed:.3f}s"
    assert re.search(r"IMAGE_SUBSCRIBER complete .*frames=40\b", rust_output), rust_output
    assert re.search(
        r"IMAGE_CONSUMER complete .*frames=3 gaps=[1-9][0-9]* "
        r"estimated_dropped=[1-9][0-9]*",
        slow_output,
    ), slow_output
    assert "IMAGE_CONSUMER_HOLD complete" in stalled_output, stalled_output

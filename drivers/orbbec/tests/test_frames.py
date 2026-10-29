"""Pure in-memory tests for the Orbbec StampedBytes application codec."""

from __future__ import annotations

import struct
import unittest

import numpy as np

from orbbec_frames import (
    FRAME_HEADER_BYTES,
    OrbbecFrame,
    depth_metres,
    parse_frame,
)
from rbt.tf_math import FrameId


def software_fixture(
    *,
    kind: int = 2,
    pixel_format: int = 4,
    width: int = 2,
    height: int = 2,
    stride: int = 4,
    unit_scale: float = 0.001,
    data: bytes = b"\x01\x00\x02\x00\x03\x00\x04\x00",
) -> bytearray:
    """Build a software-only wire fixture; it is not data from a camera."""
    encoded = bytearray(FRAME_HEADER_BYTES + len(data))
    encoded[:8] = b"RBTORB01"
    struct.pack_into("<IIIII", encoded, 8, kind, pixel_format, width, height, stride)
    struct.pack_into("<QdQ", encoded, 32, 23, unit_scale, len(data))
    encoded[64:128] = FrameId("orbbec335/depth_optical").to_bytes()
    encoded[FRAME_HEADER_BYTES:] = data
    return encoded


class OrbbecFrameTests(unittest.TestCase):
    def test_depth_little_endian_scale_and_padded_rows(self) -> None:
        fixture = software_fixture(
            width=3,
            height=2,
            stride=8,
            data=struct.pack("<3H2x3H2x", 1000, 0, 2500, 2, 3, 4),
        )

        frame = parse_frame(fixture)

        self.assertIsInstance(frame, OrbbecFrame)
        self.assertEqual(frame.shape, (2, 3))
        self.assertEqual(frame.pixels.dtype.str, "<u2")
        self.assertEqual(frame.pixels.strides, (8, 2))
        self.assertFalse(frame.pixels.flags.writeable)
        self.assertEqual(frame.frame_id, "orbbec335/depth_optical")
        np.testing.assert_array_equal(frame.pixels, [[1000, 0, 2500], [2, 3, 4]])
        self.assertEqual(frame.depth_unit_m, 0.001)
        metres = depth_metres(frame)
        self.assertTrue(metres.flags.owndata)
        np.testing.assert_allclose(metres, [[1.0, 0.0, 2.5], [0.002, 0.003, 0.004]])
        with self.assertRaises(ValueError):
            frame.pixels[0, 0] = 0

    def test_point_array_is_n_by_three_and_keeps_little_endian_values(self) -> None:
        fixture = software_fixture(
            kind=5,
            pixel_format=5,
            width=2,
            height=1,
            stride=24,
            unit_scale=1.0,
            data=struct.pack("<6f", 1, 2, 3, float("nan"), 5, 6),
        )

        frame = parse_frame(fixture)

        self.assertEqual(frame.kind, "points")
        self.assertEqual(frame.shape, (2, 3))
        self.assertEqual(frame.pixels.dtype.str, "<f4")
        self.assertFalse(np.isfinite(frame.pixels).all(axis=1)[1])

    def test_rejects_bad_magic_reserved_bytes_and_truncated_payloads(self) -> None:
        bad_magic = software_fixture()
        bad_magic[0] ^= 1
        with self.assertRaisesRegex(ValueError, "magic"):
            parse_frame(bad_magic)

        for offset in (28, 56):
            with self.subTest(offset=offset):
                bad_reserved = software_fixture()
                bad_reserved[offset] = 1
                with self.assertRaisesRegex(ValueError, "reserved"):
                    parse_frame(bad_reserved)

        with self.assertRaisesRegex(ValueError, "header"):
            parse_frame(bytes(FRAME_HEADER_BYTES - 1))
        with self.assertRaisesRegex(ValueError, "length"):
            parse_frame(software_fixture()[:-1])

    def test_rejects_invalid_kind_and_kind_format_pair(self) -> None:
        with self.assertRaisesRegex(ValueError, "kind 99"):
            parse_frame(software_fixture(kind=99))
        with self.assertRaisesRegex(ValueError, "do not match"):
            parse_frame(software_fixture(kind=2, pixel_format=1))
        with self.assertRaisesRegex(ValueError, "format 99"):
            parse_frame(software_fixture(pixel_format=99))

    def test_rejects_malformed_frame_id_and_payload_lengths(self) -> None:
        bad_frame_id = software_fixture()
        bad_frame_id[64:128] = b"orbbec\0bad" + bytes(54)
        with self.assertRaisesRegex(ValueError, "frame id"):
            parse_frame(bad_frame_id)

        with self.assertRaisesRegex(ValueError, "size"):
            parse_frame(software_fixture(data=b""))

        bad_size = software_fixture()
        struct.pack_into("<Q", bad_size, 48, 7)
        with self.assertRaisesRegex(ValueError, "length"):
            parse_frame(bad_size)

    def test_rejects_size_overflow_and_invalid_dimensions_or_stride(self) -> None:
        size_overflow = software_fixture()
        struct.pack_into("<Q", size_overflow, 48, (1 << 64) - 1)
        with self.assertRaisesRegex(ValueError, "32 MiB"):
            parse_frame(size_overflow)

        bad_dimensions = software_fixture(width=0)
        with self.assertRaisesRegex(ValueError, "dimensions"):
            parse_frame(bad_dimensions)

        bad_stride = software_fixture(stride=3)
        with self.assertRaisesRegex(ValueError, "stride"):
            parse_frame(bad_stride)

    def test_rejects_nonpositive_depth_scale(self) -> None:
        with self.assertRaisesRegex(ValueError, "positive"):
            parse_frame(software_fixture(unit_scale=0.0))
        with self.assertRaisesRegex(ValueError, "finite"):
            parse_frame(software_fixture(unit_scale=float("inf")))

    def test_imu_requires_three_finite_little_endian_floats(self) -> None:
        for kind, pixel_format in ((6, 6), (7, 7)):
            with self.subTest(kind=kind):
                frame = parse_frame(
                    software_fixture(
                        kind=kind,
                        pixel_format=pixel_format,
                        width=1,
                        height=1,
                        stride=12,
                        unit_scale=1.0,
                        data=struct.pack("<3f", 1.25, -2.5, 3.75),
                    )
                )
                self.assertEqual(frame.shape, (3,))
                self.assertEqual(frame.pixels.dtype.str, "<f4")
                np.testing.assert_array_equal(frame.pixels, [1.25, -2.5, 3.75])

                for values in (
                    (1.0, float("nan"), 3.0),
                    (1.0, 2.0, float("inf")),
                ):
                    with self.subTest(values=values):
                        malformed = software_fixture(
                            kind=kind,
                            pixel_format=pixel_format,
                            width=1,
                            height=1,
                            stride=12,
                            unit_scale=1.0,
                            data=struct.pack("<3f", *values),
                        )
                        with self.assertRaisesRegex(ValueError, "finite floats"):
                            parse_frame(malformed)


if __name__ == "__main__":
    unittest.main()

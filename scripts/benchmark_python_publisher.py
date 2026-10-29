#!/usr/bin/env python3
"""Small publisher timing sanity check.

This intentionally keeps payload filling separate from loan, mapping, and send
timings.  It is a regression guard for the public zero-copy API, not a
throughput benchmark.
"""

from __future__ import annotations

import gc
import statistics
import sys
import time
import uuid

import numpy as np

import rbt


SAMPLES = 5
MIB = 1024 * 1024


def unique_service(label: str) -> str:
    return f"rbt/python/{label}-{uuid.uuid4().hex}"


def pattern_fill(array: np.ndarray, sequence: int) -> None:
    flat = array.reshape(-1)
    for offset in range(256):
        flat[offset::256] = (sequence + offset) & 0xFF
    del flat


def median_ns(values: list[int]) -> int:
    return int(statistics.median(values))


def image_case(
    label: str,
    width: int,
    height: int,
    encoding: rbt.PixelFormat,
    stride: int,
    payload_bytes: int,
) -> dict[str, int | str]:
    publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service(f"publisher-bench-{label}")),
        maximum_payload_bytes=payload_bytes,
    )

    # Mapping is timed without touching the payload.  This keeps the result
    # sensitive to accidental copy-on-map implementations while excluding
    # the cost of filling a large frame.
    mapping_times: list[int] = []
    loan_times: list[int] = []
    send_times: list[int] = []
    fill_times: list[int] = []
    for sequence in range(SAMPLES):
        started = time.perf_counter_ns()
        loan = publisher.loan(
            width,
            height,
            encoding,
            stride=stride,
            sequence=sequence,
            timestamp_ns=sequence,
        )
        loan_done = time.perf_counter_ns()
        array = loan.numpy()
        mapped = time.perf_counter_ns()
        assert array.__array_interface__["data"][0] == loan._debug_payload_address()
        assert array.flags.writeable and not array.flags.owndata
        del array
        gc.collect()
        sent = time.perf_counter_ns()
        loan.send()
        send_done = time.perf_counter_ns()
        loan_times.append(loan_done - started)
        mapping_times.append(mapped - loan_done)
        send_times.append(send_done - sent)

        loan = publisher.loan(
            width,
            height,
            encoding,
            stride=stride,
            sequence=sequence,
            timestamp_ns=sequence,
        )
        array = loan.numpy()
        mapping_done = time.perf_counter_ns()
        pattern_fill(array, sequence)
        fill_done = time.perf_counter_ns()
        del array
        gc.collect()
        send_started = time.perf_counter_ns()
        loan.send()
        send_finished = time.perf_counter_ns()
        # A separate fill sample prevents the large write from contaminating
        # the mapping-only measurements above.
        fill_times.append(fill_done - mapping_done)
        send_times.append(send_finished - send_started)

    mapping_median = median_ns(mapping_times)
    return {
        "label": label,
        "payload_bytes": payload_bytes,
        "loan_ns": median_ns(loan_times),
        "mapping_ns": mapping_median,
        "fill_ns": median_ns(fill_times),
        "send_ns": median_ns(send_times),
    }


def main() -> int:
    cases = [
        image_case("64b", 64, 1, rbt.PixelFormat.GRAY8, 64, 64),
        image_case("6mib", 1_920, 1_080, rbt.PixelFormat.RGB8, 5_760, 6_220_800),
        image_case("20mib", 20 * MIB, 1, rbt.PixelFormat.GRAY8, 20 * MIB, 20 * MIB),
    ]

    small_mapping = int(cases[0]["mapping_ns"])
    large_mapping = int(cases[-1]["mapping_ns"])
    # The ratio is intentionally generous for timer noise on short mappings,
    # but still catches a payload-sized allocation/copy in numpy().
    mapping_ratio_limit = 250
    assert large_mapping <= max(small_mapping, 1) * mapping_ratio_limit, (
        f"mapping scaled with payload: 64B={small_mapping}ns "
        f"20MiB={large_mapping}ns limit={mapping_ratio_limit}x"
    )

    print("RBT_PYTHON_PUBLISHER_BENCH mapping_ratio_limit=250")
    for case in cases:
        print(
            "RBT_PYTHON_PUBLISHER_BENCH"
            f" label={case['label']} payload_bytes={case['payload_bytes']}"
            f" loan_ns={case['loan_ns']} mapping_ns={case['mapping_ns']}"
            f" fill_ns={case['fill_ns']} send_ns={case['send_ns']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())

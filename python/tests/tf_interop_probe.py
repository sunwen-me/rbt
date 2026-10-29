#!/usr/bin/env python3
"""Cross-language typed TF transport and math interoperability probe."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import time
import uuid

from rbt import StreamProfile
from rbt.tf import (
    DynamicRecord,
    DynamicTfPublisher,
    DynamicTfSubscriber,
    Quaternion,
    StaticEdge,
    StaticSet,
    StaticTfClient,
    StaticTfProvider,
    TfError,
    Transform3,
    TransformBuffer,
    Vec3,
    dynamic_transform_descriptor,
    static_snapshot_descriptor,
    static_update_descriptor,
)
from rbt.time import (
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockMappingCalibration,
    ClockSourceId,
    Timestamp,
)

MAX_STATIC_TRANSFORMS = 64
MAX_DRAIN_BUDGET = 64
SLOT_DYNAMIC = "interop-dynamic"
SLOT_STATIC = "interop-static"


def _fixture_clock(kind: ClockKind, source_marker: int, generation: int = 0) -> ClockIdentity:
    return ClockIdentity(kind, ClockSourceId(bytes([source_marker]) + bytes(47)), generation)


def _device_clock() -> ClockIdentity:
    return ClockIdentity(
        ClockKind.DEVICE,
        ClockSourceId.from_device_uuid(bytes([42]) * 16),
        7,
    )


def _validate_ids(args: argparse.Namespace) -> tuple[uuid.UUID, uuid.UUID, uuid.UUID]:
    values = []
    for name in ("static_id", "dynamic_id", "snapshot_id"):
        value = uuid.UUID(getattr(args, name))
        if value.int == 0:
            raise ValueError(f"{name} must be a non-nil UUID")
        values.append(value)
    static_id, dynamic_id, snapshot_id = values
    if len(set(values)) != len(values):
        raise ValueError("static, dynamic, and snapshot IDs must be distinct")
    return static_id, dynamic_id, snapshot_id


def _descriptors(
    static_id: uuid.UUID,
    dynamic_id: uuid.UUID,
    snapshot_id: uuid.UUID,
) -> tuple[object, object, object]:
    profile = StreamProfile.CONTROL
    return (
        static_update_descriptor(f"rbt/cap/{static_id}/stream", profile),
        dynamic_transform_descriptor(f"rbt/cap/{dynamic_id}/stream", profile),
        static_snapshot_descriptor(f"rbt/cap/{snapshot_id}/rpc", MAX_STATIC_TRANSFORMS),
    )


def _initial_static_set(revision: int = 10) -> StaticSet:
    stamp = Timestamp(_device_clock(), 0)
    identity = Transform3.identity()
    return StaticSet(
        revision,
        stamp,
        0,
        (
            StaticEdge("base_link", "camera_link", Transform3(Vec3(1.0, 0.0, 0.0), Quaternion.identity())),
            StaticEdge("camera_link", "camera_optical", identity),
        ),
    )


def _touch(path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f"{path.name}.tmp-{uuid.uuid4().hex}")
    temporary.write_text("ready\n", encoding="ascii")
    temporary.replace(path)


def _wait_for(path: pathlib.Path, deadline: float, description: str) -> None:
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.01)
    raise TimeoutError(f"timed out waiting for {description}: {path}")


def _remaining(deadline: float) -> float:
    result = deadline - time.monotonic()
    if result <= 0:
        raise TimeoutError("TF interoperability probe timed out")
    return result


def _serve_until_first_snapshot(
    provider: StaticTfProvider,
    deadline: float,
) -> None:
    while True:
        status = provider.serve_one(min(0.05, _remaining(deadline)))
        if status is None:
            continue
        if status != "success":
            raise RuntimeError(f"initial static snapshot request failed: {status}")
        return


def _publish(args: argparse.Namespace) -> None:
    static_id, dynamic_id, snapshot_id = _validate_ids(args)
    static_descriptor, dynamic_descriptor, snapshot_descriptor = _descriptors(
        static_id,
        dynamic_id,
        snapshot_id,
    )
    initial = _initial_static_set()
    provider = StaticTfProvider.open(
        static_descriptor,
        snapshot_descriptor,
        static_id,
        snapshot_id,
        initial,
        MAX_STATIC_TRANSFORMS,
    )
    publisher = DynamicTfPublisher.open(dynamic_descriptor, dynamic_id)
    deadline = time.monotonic() + args.timeout_sec
    try:
        _touch(args.ready)
        _wait_for(args.peer_ready, deadline, "consumer listeners")
        provider.replace(_initial_static_set(11))
        identity = Quaternion.identity()
        quarter_turn = Quaternion(0.0, 0.0, math.sin(math.pi / 4.0), math.cos(math.pi / 4.0))
        dynamic_samples = 0
        publisher.publish(
            DynamicRecord(
                "odom",
                "base_link",
                Timestamp(_device_clock(), 100_000_000),
                0,
                3,
                Transform3(Vec3(0.0, 0.0, 0.0), identity),
            )
        )
        dynamic_samples += 1
        publisher.publish(
            DynamicRecord(
                "odom",
                "base_link",
                Timestamp(_device_clock(), 200_000_000),
                0,
                3,
                Transform3(Vec3(2.0, 0.0, 0.0), quarter_turn),
            )
        )
        dynamic_samples += 1

        _serve_until_first_snapshot(provider, deadline)

        while not args.done.exists():
            provider.serve_one(min(0.05, _remaining(deadline)))

        provider_metrics = provider.current_metrics
        update_metrics = provider_metrics["update"]["publisher"]
        snapshot_metrics = provider_metrics["snapshot_rpc"]["server"]
        dynamic_metrics = publisher.current_metrics["publisher"]
        revision = provider.current.revision
        static_update_samples = update_metrics["send_successes"]
        snapshot_responses = snapshot_metrics["responses_sent"]
        pending_rpc = snapshot_metrics["requests_current"]
        loans_in_use = update_metrics["loans_in_use"] + dynamic_metrics["loans_in_use"]
        if (
            revision != 11
            or dynamic_samples != 2
            or dynamic_metrics["send_successes"] != 2
            or static_update_samples < 1
            or snapshot_responses < 1
            or pending_rpc != 0
            or loans_in_use != 0
        ):
            raise RuntimeError("publisher-side TF fixture or RPC/loan accounting differs")
        print(
            json.dumps(
                {
                    "status": "PASS",
                    "language": "python",
                    "static_snapshot": "PASS",
                    "static_update": "PASS",
                    "dynamic": "PASS",
                    "lookup": "PASS",
                    "static_revision": revision,
                    "dynamic_samples": dynamic_samples,
                    "borrowed_samples": 0,
                    "pending_rpc": pending_rpc,
                    "static_update_samples_published": static_update_samples,
                    "snapshot_responses": snapshot_responses,
                    "dynamic_send_successes": dynamic_metrics["send_successes"],
                    "loans_in_use": loans_in_use,
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
    finally:
        publisher.close()
        provider.close()


def _provider_revision(buffer: TransformBuffer, authority_id: uuid.UUID) -> int | None:
    metrics = buffer.pressure_snapshot()
    for provider in metrics["providers"]:
        if provider["authority"]["physical"] == str(authority_id):
            return provider["static_revision"]
    return None


def _consume(args: argparse.Namespace) -> None:
    static_id, dynamic_id, snapshot_id = _validate_ids(args)
    _wait_for(args.ready, time.monotonic() + args.timeout_sec, "publisher endpoints")
    static_descriptor, dynamic_descriptor, snapshot_descriptor = _descriptors(
        static_id,
        dynamic_id,
        snapshot_id,
    )
    buffer = TransformBuffer()
    dynamic = DynamicTfSubscriber.open(dynamic_descriptor, dynamic_id, SLOT_DYNAMIC, buffer)
    static = StaticTfClient.open(
        static_id,
        static_descriptor,
        snapshot_descriptor,
        snapshot_id,
        1,
        SLOT_STATIC,
        MAX_STATIC_TRANSFORMS,
        buffer,
    )
    deadline = time.monotonic() + args.timeout_sec
    try:
        _touch(args.peer_ready)
        static.refresh_snapshot(_remaining(deadline))
        static_metrics = static.current_metrics
        if static_metrics["snapshot_rpc"]["client"]["calls_completed"] != 1:
            raise RuntimeError("static snapshot RPC did not complete exactly once")
        snapshot_revision = _provider_revision(buffer, static_id)
        if snapshot_revision not in (10, 11):
            raise RuntimeError("static snapshot did not install a bounded initial revision")

        update_samples = static_metrics["update"]["subscriber"]["received_samples"]
        while update_samples < 1 or _provider_revision(buffer, static_id) != 11:
            report = static.wait_and_drain(min(0.25, _remaining(deadline)))
            if report.rejected or report.failures:
                raise RuntimeError(f"static update was rejected: {report.failures}")
            static_metrics = static.current_metrics
            update_samples = static_metrics["update"]["subscriber"]["received_samples"]
        if update_samples < 1:
            raise RuntimeError("static update stream did not install revision 11")

        dynamic_received = 0
        dynamic_accepted = 0
        while dynamic_accepted < 2:
            report = dynamic.wait_and_drain(min(0.25, _remaining(deadline)))
            dynamic_received += report.received
            dynamic_accepted += report.accepted
            if report.rejected or report.failures:
                raise RuntimeError(f"dynamic TF sample was rejected: {report.failures}")
        if dynamic_received != 2 or dynamic_accepted != 2:
            raise RuntimeError("dynamic stream did not yield exactly two fixture samples")

        result = buffer.lookup_at(
            "odom",
            "camera_optical",
            Timestamp(_device_clock(), 150_000_000),
            uncertainty_ns=0,
        )
        mapped_point = result.transform.apply_point(Vec3(0.0, 0.0, 0.0))
        expected_point = Vec3(1.0 + math.sqrt(0.5), math.sqrt(0.5), 0.0)
        if not _close_vec(mapped_point, expected_point):
            raise RuntimeError(f"lookup point differs from fixture: {mapped_point}")
        round_trip = result.transform.inverse().apply_point(mapped_point)
        if not _close_vec(round_trip, Vec3(0.0, 0.0, 0.0)):
            raise RuntimeError(f"inverse round trip differs from origin: {round_trip}")

        dynamic_metrics = dynamic.current_metrics
        static_metrics = static.current_metrics
        dynamic_borrows = dynamic_metrics["subscriber"]["borrowed_samples"]
        static_borrows = static_metrics["update"]["subscriber"]["borrowed_samples"]
        pending_rpc = static_metrics["snapshot_rpc"]["client"]["in_flight_current"]
        response_guards = static_metrics["snapshot_rpc"]["client"]["response_guards_current"]
        if dynamic_borrows or static_borrows or pending_rpc or response_guards:
            raise RuntimeError("TF native transport retained a sample guard or RPC request")

        _touch(args.done)
        print(
            json.dumps(
                {
                    "status": "PASS",
                    "language": "python",
                    "static_snapshot": "PASS",
                    "static_update": "PASS",
                    "dynamic": "PASS",
                    "lookup": "PASS",
                    "static_revision": 11,
                    "dynamic_samples": 2,
                    "borrowed_samples": 0,
                    "pending_rpc": 0,
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
    finally:
        static.close()
        dynamic.close()


def _close_vec(actual: Vec3, expected: Vec3) -> bool:
    return all(
        abs(left - right) <= 1.0e-12
        for left, right in zip((actual.x, actual.y, actual.z), (expected.x, expected.y, expected.z))
    )


def _close_quaternion(actual: Quaternion, expected: Quaternion) -> bool:
    dot = sum(
        left * right
        for left, right in zip(
            (actual.x, actual.y, actual.z, actual.w),
            (expected.x, expected.y, expected.z, expected.w),
        )
    )
    return abs(abs(dot) - 1.0) <= 1.0e-12


def _fixture_buffer() -> tuple[TransformBuffer, ClockIdentity, ClockIdentity, ClockIdentity]:
    device_clock = _device_clock()
    system_clock = _fixture_clock(ClockKind.SYSTEM_REALTIME, 43)
    host_clock = _fixture_clock(ClockKind.HOST_MONOTONIC, 44)
    buffer = TransformBuffer()
    authority = buffer.bind_provider("math-fixture", uuid.UUID("00112233-4455-6677-8899-aabbccddeeff"))
    identity = Quaternion.identity()
    buffer.apply_static_snapshot(
        authority,
        StaticSet(
            1,
            Timestamp(device_clock, 0),
            0,
            (
                StaticEdge("base_link", "camera_link", Transform3(Vec3(1.0, 0.0, 0.0), identity)),
                StaticEdge("camera_link", "camera_optical", Transform3.identity()),
            ),
        ),
    )
    buffer.ingest_dynamic(
        authority,
        DynamicRecord(
            "odom",
            "base_link",
            Timestamp(device_clock, 100_000_000),
            0,
            3,
            Transform3.identity(),
        ),
    )
    buffer.ingest_dynamic(
        authority,
        DynamicRecord(
            "odom",
            "base_link",
            Timestamp(device_clock, 200_000_000),
            0,
            3,
            Transform3(
                Vec3(2.0, 0.0, 0.0),
                Quaternion(0.0, 0.0, math.sin(math.pi / 4.0), math.cos(math.pi / 4.0)),
            ),
        ),
    )
    return buffer, device_clock, system_clock, host_clock


def _semantics() -> None:
    translated = Transform3(Vec3(1.0, 0.0, 0.0), Quaternion.identity())
    direction_point = translated.apply_point(Vec3(0.0, 0.0, 1.0))
    inverse_point = translated.inverse().apply_point(direction_point)
    if not _close_vec(direction_point, Vec3(1.0, 0.0, 1.0)):
        raise RuntimeError("transform direction fixture failed")
    if not _close_vec(inverse_point, Vec3(0.0, 0.0, 1.0)):
        raise RuntimeError("inverse transform fixture failed")

    identity = Quaternion.identity()
    quarter_turn = Quaternion(0.0, 0.0, math.sin(math.pi / 4.0), math.cos(math.pi / 4.0))
    interpolated = Transform3.identity().interpolate(
        Transform3(Vec3(2.0, 0.0, 0.0), quarter_turn),
        0.5,
    )
    expected_mid_rotation = Quaternion(0.0, 0.0, math.sin(math.pi / 8.0), math.cos(math.pi / 8.0))
    if not _close_vec(interpolated.translation, Vec3(1.0, 0.0, 0.0)):
        raise RuntimeError("transform interpolation translation fixture failed")
    if not _close_quaternion(interpolated.rotation, expected_mid_rotation):
        raise RuntimeError("transform interpolation rotation fixture failed")

    map_from_odom = Transform3(Vec3(10.0, 0.0, 0.0), identity)
    base_from_camera = Transform3(Vec3(1.0, 0.0, 0.0), identity)
    chain = map_from_odom.compose(interpolated).compose(base_from_camera)
    chain_point = chain.apply_point(Vec3(0.0, 0.0, 0.0))
    expected_chain = Vec3(11.0 + math.sqrt(0.5), math.sqrt(0.5), 0.0)
    if not _close_vec(chain_point, expected_chain):
        raise RuntimeError("transform chain direction fixture failed")

    buffer, device_clock, system_clock, host_clock = _fixture_buffer()
    query = Timestamp(system_clock, 150_000_000)
    try:
        buffer.lookup_at("odom", "camera_optical", query, uncertainty_ns=0)
    except TfError as error:
        wrong_clock_code = error.code
    else:
        raise RuntimeError("lookup without an explicit clock mapping unexpectedly succeeded")
    if wrong_clock_code != "time_clock_mapping_unavailable":
        raise RuntimeError(f"wrong-clock lookup returned unexpected error: {wrong_clock_code}")

    mapping = ClockMapping(
        ClockMappingCalibration(
            system_clock,
            device_clock,
            0,
            0,
            1,
            1,
            -1_000_000_000,
            1_000_000_000,
            b"\x01" + bytes(15),
            Timestamp(host_clock, 0),
            Timestamp(host_clock, 100),
            0,
        )
    )
    mapped = buffer.lookup_at(
        "odom",
        "camera_optical",
        query,
        uncertainty_ns=0,
        mappings=(mapping,),
        now=Timestamp(host_clock, 1),
    )
    mapped_point = mapped.transform.apply_point(Vec3(0.0, 0.0, 0.0))
    if not _close_vec(mapped_point, Vec3(1.0 + math.sqrt(0.5), math.sqrt(0.5), 0.0)):
        raise RuntimeError("mapped-clock lookup fixture failed")
    try:
        buffer.lookup_at(
            "odom",
            "camera_optical",
            query,
            uncertainty_ns=0,
            mappings=(mapping,),
            now=Timestamp(host_clock, 100),
        )
    except TfError as error:
        expired_code = error.code
    else:
        raise RuntimeError("expired clock mapping unexpectedly succeeded")
    if expired_code != "time_clock_mapping_expired":
        raise RuntimeError(f"expired mapping returned unexpected error: {expired_code}")

    print(
        json.dumps(
            {
                "language": "python",
                "direction_point": [direction_point.x, direction_point.y, direction_point.z],
                "inverse_point": [inverse_point.x, inverse_point.y, inverse_point.z],
                "chain_point": [chain_point.x, chain_point.y, chain_point.z],
                "interpolated_translation": [
                    interpolated.translation.x,
                    interpolated.translation.y,
                    interpolated.translation.z,
                ],
                "interpolated_quaternion_xyzw": [
                    interpolated.rotation.x,
                    interpolated.rotation.y,
                    interpolated.rotation.z,
                    interpolated.rotation.w,
                ],
                "wrong_clock_error": "clock_mapping_unavailable",
                "expired_mapping_error": "clock_mapping_expired",
                "mapped_point": [mapped_point.x, mapped_point.y, mapped_point.z],
            },
            separators=(",", ":"),
        ),
        flush=True,
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("semantics")
    interop = commands.add_parser("interop")
    roles = interop.add_subparsers(dest="role", required=True)
    for role in ("publish", "consume"):
        role_parser = roles.add_parser(role)
        role_parser.add_argument("--static-id", required=True)
        role_parser.add_argument("--dynamic-id", required=True)
        role_parser.add_argument("--snapshot-id", required=True)
        role_parser.add_argument("--ready", required=True, type=pathlib.Path)
        role_parser.add_argument("--peer-ready", required=True, type=pathlib.Path)
        role_parser.add_argument("--done", required=True, type=pathlib.Path)
        role_parser.add_argument("--timeout-sec", required=True, type=float)
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    if args.command == "semantics":
        _semantics()
        return
    if not math.isfinite(args.timeout_sec) or args.timeout_sec <= 0.0:
        raise ValueError("timeout-sec must be finite and positive")
    if args.role == "publish":
        _publish(args)
    else:
        _consume(args)


if __name__ == "__main__":
    main()

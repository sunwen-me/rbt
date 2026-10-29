import copy
import types
import unittest

from scripts import tf_process_probe


CAPABILITY_IDS = {
    "dynamic": "dynamic-capability",
    "static_update": "static-update-capability",
    "static_snapshot": "static-snapshot-capability",
}


def live_stream(service_name, *, current=1, maximum=1, high_water=1, failures=0):
    return {
        "service_name": service_name,
        "profile": "control",
        "semantic": {"queue_depth": 4},
        "resources": {"max_borrowed": maximum},
        "subscriber": {
            "borrowed_samples": current,
            "borrowed_high_water": high_water,
            "borrow_failures": failures,
        },
    }


def consumer_child(*, retirement=None, extra_events=()):
    dynamic_service = f"rbt/cap/{CAPABILITY_IDS['dynamic']}/stream"
    update_service = f"rbt/cap/{CAPABILITY_IDS['static_update']}/stream"
    snapshot_service = f"rbt/cap/{CAPABILITY_IDS['static_snapshot']}/rpc"
    pressure = {
        "capability_ids": dict(CAPABILITY_IDS),
        "streams": [
            {
                **live_stream(dynamic_service),
                "subscriber": {"received_samples": 2},
            },
            {
                **live_stream(update_service),
                "subscriber": {"received_samples": 1},
            },
        ],
        "rpc": [
            {
                "service_name": snapshot_service,
                "capability_instance": CAPABILITY_IDS["static_snapshot"],
                "client": {"calls_completed": 1},
            }
        ],
    }
    events = [
        {
            "event": "activation",
            "role": "consumer",
            "activation_id": 7,
            "dynamic_physical_id": CAPABILITY_IDS["dynamic"],
            "static_physical_id": CAPABILITY_IDS["static_update"],
            "static_snapshot_physical_id": CAPABILITY_IDS["static_snapshot"],
        },
        {
            "event": "consumer_summary",
            "role": "consumer",
            "activation_id": 7,
            "transport_pressure": pressure,
        },
        {"event": "deactivation", "role": "consumer", "activation_id": 7},
    ]
    events.extend(extra_events)
    if retirement is not None:
        events.append(
            {
                "event": "activation_transport_retired",
                "role": "consumer",
                "activation_id": 7,
                "transport_pressure": {
                    "capability_ids": dict(CAPABILITY_IDS),
                    "streams": [],
                    "rpc": [],
                },
                "all_sources_released": True,
                **retirement,
            }
        )
    return types.SimpleNamespace(
        role="consumer-0",
        process=types.SimpleNamespace(pid=1234),
        events=lambda: copy.deepcopy(events),
    )


class ProcessPressureTests(unittest.TestCase):
    def test_live_guard_and_actual_matching_retirement_pass(self):
        snapshot = live_stream("rbt/cap/dynamic-capability/stream")
        result = tf_process_probe.native_transport_retirement_assertion(
            [consumer_child(retirement={})]
        )

        self.assertTrue(tf_process_probe.native_stream_guard_bounded(snapshot))
        self.assertTrue(result["pass"], result["errors"])
        self.assertEqual(result["active_endpoint_evidence"], [
            {
                "pid": 1234,
                "activation_id": 7,
                "dynamic": True,
                "static_update": True,
                "static_snapshot": True,
            }
        ])

    def test_live_guard_over_limit_fails(self):
        self.assertFalse(
            tf_process_probe.native_stream_guard_bounded(
                live_stream("dynamic", current=2, maximum=1)
            )
        )

    def test_live_guard_high_water_over_limit_fails(self):
        self.assertFalse(
            tf_process_probe.native_stream_guard_bounded(
                live_stream("dynamic", high_water=2, maximum=1)
            )
        )

    def test_live_guard_borrow_failure_fails(self):
        self.assertFalse(
            tf_process_probe.native_stream_guard_bounded(
                live_stream("dynamic", failures=1)
            )
        )

    def test_live_guard_missing_gauges_fail(self):
        for section, field in (
            ("resources", "max_borrowed"),
            ("subscriber", "borrowed_samples"),
            ("subscriber", "borrowed_high_water"),
            ("subscriber", "borrow_failures"),
        ):
            with self.subTest(section=section, field=field):
                snapshot = live_stream("dynamic")
                del snapshot[section][field]
                self.assertFalse(tf_process_probe.native_stream_guard_bounded(snapshot))

    def test_missing_retirement_fails(self):
        result = tf_process_probe.native_transport_retirement_assertion([consumer_child()])
        self.assertFalse(result["pass"])
        self.assertTrue(any("expected one" in error for error in result["errors"]))

    def test_duplicate_retirement_fails(self):
        retirement = {
            "event": "activation_transport_retired",
            "role": "consumer",
            "activation_id": 7,
            "transport_pressure": {
                "capability_ids": dict(CAPABILITY_IDS),
                "streams": [],
                "rpc": [],
            },
            "all_sources_released": True,
        }
        child = consumer_child(extra_events=(retirement, retirement))
        result = tf_process_probe.native_transport_retirement_assertion([child])
        self.assertFalse(result["pass"])
        self.assertTrue(any("duplicate retirement" in error for error in result["errors"]))

    def test_nonempty_retirement_fails(self):
        for field in ("streams", "rpc"):
            with self.subTest(field=field):
                child = consumer_child(retirement={})
                events = child.events()
                events[-1]["transport_pressure"][field].append({"still_live": True})
                child.events = lambda: copy.deepcopy(events)
                result = tf_process_probe.native_transport_retirement_assertion([child])
                self.assertFalse(result["pass"])

    def test_mismatched_retired_capability_ids_fail(self):
        child = consumer_child(retirement={})
        events = child.events()
        events[-1]["transport_pressure"]["capability_ids"]["dynamic"] = "wrong-id"
        child.events = lambda: copy.deepcopy(events)
        result = tf_process_probe.native_transport_retirement_assertion([child])
        self.assertFalse(result["pass"])
        self.assertTrue(any("mismatched capability IDs" in error for error in result["errors"]))

    def test_unobserved_native_endpoint_fails(self):
        child = consumer_child(retirement={})
        events = child.events()
        events[1]["transport_pressure"]["rpc"][0]["client"]["calls_completed"] = 0
        child.events = lambda: copy.deepcopy(events)
        result = tf_process_probe.native_transport_retirement_assertion([child])
        self.assertFalse(result["pass"])
        self.assertTrue(any("static_snapshot data evidence" in error for error in result["errors"]))

    def test_pressure_checks_accept_bounded_pre_join_rpc_and_publisher_snapshot(self):
        child = consumer_child(retirement={})
        client = {
            "in_flight_current": 1,
            "in_flight_high_water": 1,
            "response_guards_current": 1,
            "response_guards_high_water": 1,
        }
        publisher = {
            "service_name": "rbt/cap/provider/stream",
            "profile": "control",
            "semantic": {"queue_depth": 4},
            "resources": {"max_borrowed": 1},
            "subscriber": None,
            "publisher": {"send_attempts": 4, "send_successes": 4, "queue_full": 0},
        }
        observations = {
            "streams": [
                publisher,
                live_stream("rbt/cap/consumer/stream", current=1, high_water=1),
            ],
            "rpc": [
                {
                    "resources": {"max_in_flight": 2, "max_borrowed_responses": 1},
                    "client": client,
                },
                {"resources": {"max_clients": 2}, "server": {"requests_current": 1}},
            ],
            "pre_join_rpc": [{"client": client}],
            "buffers": [{"stored_samples": 0, "graph": []}],
            "slow_subscribers": [],
            "provider_queue_full_after_subscribe": True,
            "max_provider_queue_full": 1,
            "provider_publisher_samples_with_two_receiving_consumers": 1,
            "static_updates_observed": 1,
            "native_transport_retirement": tf_process_probe.native_transport_retirement_assertion(
                [child]
            ),
        }

        checks = tf_process_probe.pressure_assertions(observations)
        self.assertTrue(checks["native_stream_guards_bounded"])
        self.assertTrue(checks["native_rpc_pending_and_guards_bounded"])
        self.assertTrue(checks["native_rpc_server_requests_bounded"])
        self.assertFalse(checks["native_rpc_zero_at_pre_join_observation"])
        self.assertTrue(checks["native_transport_retired_after_join"])

        malformed = copy.deepcopy(observations)
        malformed["streams"].append(
            {
                "profile": "control",
                "semantic": {"queue_depth": 4},
                "resources": {"max_borrowed": 1},
            }
        )
        self.assertFalse(
            tf_process_probe.pressure_assertions(malformed)["native_stream_guards_bounded"]
        )
        malformed_publisher = copy.deepcopy(observations)
        malformed_publisher["streams"][0]["publisher"] = {}
        self.assertFalse(
            tf_process_probe.pressure_assertions(malformed_publisher)[
                "native_stream_guards_bounded"
            ]
        )

    def test_retirement_requires_explicit_release_flag_and_arrays(self):
        for field in ("all_sources_released", "streams", "rpc"):
            with self.subTest(field=field):
                child = consumer_child(retirement={})
                events = child.events()
                if field == "all_sources_released":
                    del events[-1][field]
                else:
                    del events[-1]["transport_pressure"][field]
                child.events = lambda: copy.deepcopy(events)
                result = tf_process_probe.native_transport_retirement_assertion([child])
                self.assertFalse(result["pass"])


if __name__ == "__main__":
    unittest.main()

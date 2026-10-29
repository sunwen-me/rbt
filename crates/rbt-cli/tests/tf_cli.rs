#![recursion_limit = "256"]

use std::{
    io::{BufRead, BufReader, Write},
    net::TcpListener,
    process::{Command, Output},
    thread,
    time::{SystemTime, UNIX_EPOCH},
};

use rbt_compose::{ComposerPhase, InspectionView, ServerMessage};
use rbt_core::{ComposerEpoch, Revision};
use rbt_time::HostClock;
use serde_json::{Value, json};

fn run_cli(arguments: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_rbt"))
        .args(arguments)
        .output()
        .expect("rbt command should start")
}

fn empty_view() -> InspectionView {
    let epoch = ComposerEpoch::new();
    InspectionView {
        epoch,
        revision: Revision::initial(epoch),
        phase: ComposerPhase::Running,
        components: Vec::new(),
        capability_topology: Vec::new(),
        process_conflicts: Vec::new(),
    }
}

fn parse_json(output: &Output) -> Value {
    serde_json::from_slice(&output.stdout).expect("command should print JSON")
}

#[test]
fn status_and_tree_json_preserve_partial_and_unavailable_composer_states() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("composer fixture should bind");
    let address = listener.local_addr().unwrap().to_string();
    let response = serde_json::to_vec(&ServerMessage::InspectResponse { view: empty_view() })
        .expect("empty composer view should serialize");
    let server = thread::spawn(move || {
        for _ in 0..2 {
            let (stream, _) = listener.accept().expect("CLI should connect to Composer");
            let mut reader = BufReader::new(stream.try_clone().unwrap());
            let mut request = String::new();
            reader.read_line(&mut request).unwrap();
            let mut stream = stream;
            stream.write_all(&response).unwrap();
            stream.write_all(b"\n").unwrap();
        }
    });

    for subcommand in ["status", "tree"] {
        let output = run_cli(&["--composer", &address, "tf", subcommand, "--json"]);
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let json = parse_json(&output);
        assert_eq!(json["availability"], "available");
        assert_eq!(json["graph_scope"], "partial");
        assert_eq!(json["graph"]["availability"], "unavailable");
        assert!(json["graph"]["edges"].is_null());
        assert!(json["providers"].as_array().unwrap().is_empty());
        assert_eq!(json["local_lookup_metrics"]["availability"], "unavailable");
        assert!(json["local_lookup_metrics"]["attempts"].is_null());
    }
    server.join().unwrap();

    for subcommand in ["status", "tree"] {
        let output = run_cli(&["--composer", "127.0.0.1:0", "tf", subcommand, "--json"]);
        assert_eq!(output.status.code(), Some(4));
        let json = parse_json(&output);
        assert_eq!(json["availability"], "unavailable");
        assert_eq!(json["composer_availability"], "unavailable");
        assert_eq!(json["graph_scope"], "partial");
        assert!(json["graph"]["edges"].is_null());
        assert!(json["local_lookup_metrics"]["attempts"].is_null());
    }
}

#[test]
fn status_and_tree_json_use_only_the_selected_local_snapshot() {
    let observed_at = HostClock::new()
        .unwrap()
        .monotonic_now()
        .expect("host monotonic clock should be available");
    let physical = uuid::Uuid::new_v4().to_string();
    let authority = json!({ "slot": "base", "physical": physical });
    let snapshot = json!({
        "observed_at": observed_at,
        "consumer_name": "localizer",
        "consumer_physical_identity": "consumer-physical-id",
        "buffer": {
            "active_providers": 1,
            "active_frames": 2,
            "static_edges": 1,
            "dynamic_edges": 0,
            "stored_samples": 0,
            "evicted_samples": 0,
            "lookup_attempts": 0,
            "lookup_successes": 0,
            "lookup_failures": 0,
            "missing_transform_count": 0,
            "extrapolation_count": 0,
            "clock_error_count": 0,
            "conflict_count": 0,
            "providers": [{
                "authority": authority,
                "static_revision": 3,
                "highest_observed_revision": 3,
                "static_tracked": true,
                "static_stale": false,
                "snapshot_age_ns": 10,
                "snapshot_fresh": true,
                "dynamic_update_age_ns": null,
                "dynamic_fresh": false,
                "active_static_edges": 1,
                "active_dynamic_edges": 0,
                "stored_samples": 0,
                "evicted_samples": 0,
                "buffer_observed_lookup_attempts": 0,
                "buffer_observed_lookup_successes": 0,
                "buffer_observed_lookup_failures": 0,
                "missing_transform_count": 0,
                "extrapolation_count": 0,
                "clock_error_count": 0,
                "conflict_count": 0,
                "resync_attempts": 0,
                "resync_failures": 0
            }],
            "graph": [{
                "parent": "world",
                "child": "base",
                "authority": authority,
                "is_static": true,
                "revision": 3,
                "transform_epoch": null,
                "stored_samples": 0,
                "conflicting_claims": 1
            }]
        }
    });
    let path = std::env::temp_dir().join(format!(
        "rbt-tf-cli-{}-{}.json",
        std::process::id(),
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    std::fs::write(&path, serde_json::to_vec(&snapshot).unwrap()).unwrap();
    let path_text = path.to_string_lossy().into_owned();

    for subcommand in ["status", "tree"] {
        let output = run_cli(&[
            "--composer",
            "127.0.0.1:0",
            "tf",
            subcommand,
            "--json",
            "--snapshot",
            &path_text,
        ]);
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );
        let json = parse_json(&output);
        assert_eq!(json["source"], "selected_local_consumer_snapshot");
        assert_eq!(json["composer_availability"], "not_queried");
        assert_eq!(json["graph_scope"], "partial");
        assert_eq!(json["local_observation"]["consumer"]["name"], "localizer");
        assert_eq!(json["graph"]["edges"][0]["parent"], "world");
        assert_eq!(json["graph"]["edges"][0]["conflicting_claims"], 1);
        assert_eq!(json["local_lookup_metrics"]["attempts"], 0);
    }
    std::fs::remove_file(path).unwrap();
}

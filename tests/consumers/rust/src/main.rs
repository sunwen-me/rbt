use std::time::Duration;

use rbt::prelude::{Capability, Requirement, Stream, StreamProfile};
use rbt::rpc::{RpcClient, RpcDescriptor, RpcServer};
use rbt::stream::{
    ReadyEvent, ReadyEventKind, WaitContext, WaitError, WaitExit, WaitProgress, WaitStats,
    WaitStatsSnapshot, WaitToken, WaitWakeHandle,
};
use rbt::types::{Detection, Frame};

const INSTALL_WAIT_TOKEN: WaitToken = WaitToken::new(7);
const INSTALL_WAIT_PROGRESS: WaitProgress = WaitProgress::Samples(1);
const INSTALL_WAIT_EVENT: ReadyEvent = ReadyEvent::synthetic_source(INSTALL_WAIT_TOKEN);

fn assert_waitset_surface() {
    let _: Option<WaitContext> = None;
    let _: Option<WaitError> = None;
    let _: Option<WaitExit> = None;
    let _: Option<WaitStats> = None;
    let _: Option<WaitStatsSnapshot> = None;
    let _: Option<WaitWakeHandle> = None;

    assert_eq!(INSTALL_WAIT_TOKEN.value(), 7);
    assert_eq!(INSTALL_WAIT_PROGRESS.samples_processed(), 1);
    assert!(!INSTALL_WAIT_PROGRESS.should_stop());
    assert_eq!(INSTALL_WAIT_EVENT.token(), INSTALL_WAIT_TOKEN);
    assert_eq!(INSTALL_WAIT_EVENT.kind(), ReadyEventKind::Source);
    assert!(INSTALL_WAIT_EVENT.is_synthetic());
}

fn assert_rpc_surface() {
    let service_name = format!("rbt/install-smoke/rpc/rust/{}", std::process::id());
    let descriptor = RpcDescriptor::for_fixed::<Frame, Detection>(service_name);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)
        .expect("installed Rust RPC server should open");
    let client = RpcClient::<Frame, Detection>::open(&descriptor)
        .expect("installed Rust RPC client should open");

    let server_thread = std::thread::spawn(move || {
        let request = server
            .receive_timeout(Duration::from_secs(2))
            .expect("installed Rust RPC server receive should succeed")
            .expect("installed Rust RPC server should receive the fixed request");
        let request_value = *request;
        let mut response = request
            .loan_response()
            .expect("installed Rust RPC response loan should succeed");
        response.sequence = request_value.sequence;
        response.value = request_value.value + 7;
        response
            .send()
            .expect("installed Rust RPC response send should succeed");
    });

    let response = client
        .call(
            Frame {
                sequence: 35,
                value: 5,
            },
            Duration::from_secs(2),
        )
        .expect("installed Rust RPC fixed call should succeed");
    assert_eq!(response.sequence, 35);
    assert_eq!(response.value, 12);
    server_thread
        .join()
        .expect("installed Rust RPC server thread should complete");
    println!("RBT_RPC_INSTALL_SMOKE_OK rust result={}", response.value);
}

struct Camera;

impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

fn main() {
    assert_waitset_surface();
    assert_rpc_surface();

    let requirement = Requirement::<Camera>::new("front");
    assert_eq!(requirement.capability_type(), "Camera");

    let descriptor = Stream::<Frame>::descriptor("rbt/install-smoke/frame", StreamProfile::Sensor);
    assert_eq!(
        descriptor.message_type().qualified_name(),
        "rbt.demo/Frame@1"
    );
    println!("installed Rust SDK {}", rbt::SDK_VERSION);
}

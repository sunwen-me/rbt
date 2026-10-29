use std::sync::atomic::{AtomicU64, Ordering};

use iceoryx2::prelude::*;

static NEXT_SERVICE_ID: AtomicU64 = AtomicU64::new(0);

fn service_name() -> ServiceName {
    let id = NEXT_SERVICE_ID.fetch_add(1, Ordering::Relaxed);
    ServiceName::new(&format!("rpc_native_corr_{}_{}", std::process::id(), id)).unwrap()
}

fn create_node() -> Node<ipc::Service> {
    NodeBuilder::new().create::<ipc::Service>().unwrap()
}

#[test]
fn retired_client_response_does_not_reach_reused_client_slot() {
    const OLD_REQUEST: u64 = 0x1111;
    const OLD_RESPONSE: u64 = 0xaaaa;
    const FRESH_REQUEST: u64 = 0x2222;
    const FRESH_RESPONSE: u64 = 0xbbbb;

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<u64, u64>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let server = service.server_builder().create().unwrap();

    let old_client = service.client_builder().create().unwrap();
    let old_pending = old_client.send_copy(OLD_REQUEST).unwrap();
    let old_request = server.receive().unwrap().expect("old request");
    assert_eq!(*old_request, OLD_REQUEST);
    let old_response = old_request
        .loan_uninit()
        .unwrap()
        .write_payload(OLD_RESPONSE);

    drop(old_pending);
    drop(old_client);

    let fresh_client = service.client_builder().create().unwrap();
    let fresh_pending = fresh_client.send_copy(FRESH_REQUEST).unwrap();
    let fresh_request = server.receive().unwrap().expect("fresh request");
    assert_eq!(*fresh_request, FRESH_REQUEST);

    // A retired response may fail to send or be accepted as a no-op. It must
    // never be delivered as the response to this new client's first request.
    let _stale_send = old_response.send();
    assert!(fresh_pending.receive().unwrap().is_none());

    fresh_request.send_copy(FRESH_RESPONSE).unwrap();
    let response = fresh_pending.receive().unwrap().expect("fresh response");
    assert_eq!(*response, FRESH_RESPONSE);

    drop(old_request);
}

#[test]
fn dropping_old_active_request_does_not_close_reused_client_channel() {
    const OLD_REQUEST: u64 = 0x3333;
    const FRESH_REQUEST: u64 = 0x4444;
    const FRESH_RESPONSE: u64 = 0xcccc;

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<u64, u64>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let server = service.server_builder().create().unwrap();

    let old_client = service.client_builder().create().unwrap();
    let old_pending = old_client.send_copy(OLD_REQUEST).unwrap();
    let old_request = server.receive().unwrap().expect("old request");
    drop(old_pending);
    drop(old_client);

    let fresh_client = service.client_builder().create().unwrap();
    let fresh_pending = fresh_client.send_copy(FRESH_REQUEST).unwrap();
    let fresh_request = server.receive().unwrap().expect("fresh request");

    assert!(!old_request.is_connected(), "old request changed client identity");
    assert!(!old_request.has_disconnect_hint());
    assert!(fresh_pending.is_connected());
    // This destructor must only retire the old connection generation.
    drop(old_request);
    assert!(fresh_pending.is_connected(), "old request closed fresh client's channel");

    fresh_request.send_copy(FRESH_RESPONSE).unwrap();
    let response = fresh_pending
        .receive()
        .unwrap()
        .expect("fresh response after old request drop");
    assert_eq!(*response, FRESH_RESPONSE);
}

#[test]
fn late_response_after_same_client_timeout_does_not_replace_next_response() {
    const FIRST_REQUEST: u64 = 0x5555;
    const LATE_RESPONSE: u64 = 0xdddd;
    const NEXT_REQUEST: u64 = 0x6666;
    const NEXT_RESPONSE: u64 = 0xeeee;

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<u64, u64>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let server = service.server_builder().create().unwrap();
    let client = service.client_builder().create().unwrap();

    let first_pending = client.send_copy(FIRST_REQUEST).unwrap();
    let first_request = server.receive().unwrap().expect("first request");
    drop(first_pending); // Model the caller timing out and abandoning this request.

    let next_pending = client.send_copy(NEXT_REQUEST).unwrap();
    let next_request = server.receive().unwrap().expect("next request");
    assert_eq!(*next_request, NEXT_REQUEST);

    let _late_send = first_request.send_copy(LATE_RESPONSE);
    assert!(next_pending.receive().unwrap().is_none());

    next_request.send_copy(NEXT_RESPONSE).unwrap();
    let response = next_pending.receive().unwrap().expect("next response");
    assert_eq!(*response, NEXT_RESPONSE);
}

#[test]
fn fixed_payload_and_user_headers_preserve_marker_bytes() {
    const REQUEST: [u8; 8] = *b"FIX-REQ!";
    const REQUEST_HEADER: [u8; 8] = *b"RQ-HEAD!";
    const RESPONSE: [u8; 8] = *b"FIX-RES!";
    const RESPONSE_HEADER: [u8; 8] = *b"RS-HEAD!";

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<[u8; 8], [u8; 8]>()
        .request_user_header::<[u8; 8]>()
        .response_user_header::<[u8; 8]>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let client = service.client_builder().create().unwrap();
    let server = service.server_builder().create().unwrap();

    let mut request = client.loan_uninit().unwrap();
    *request.user_header_mut() = REQUEST_HEADER;
    let pending = request.write_payload(REQUEST).send().unwrap();

    let active_request = server.receive().unwrap().expect("fixed request");
    assert_eq!(*active_request, REQUEST);
    assert_eq!(*active_request.user_header(), REQUEST_HEADER);

    let mut response = active_request.loan_uninit().unwrap();
    *response.user_header_mut() = RESPONSE_HEADER;
    response.write_payload(RESPONSE).send().unwrap();

    let response = pending.receive().unwrap().expect("fixed response");
    assert_eq!(*response, RESPONSE);
    assert_eq!(*response.user_header(), RESPONSE_HEADER);
}

#[test]
fn slice_payload_and_user_headers_preserve_marker_bytes() {
    const REQUEST: [u8; 8] = *b"SLI-REQ!";
    const REQUEST_HEADER: [u8; 8] = *b"SRQ-HEAD";
    const RESPONSE: [u8; 8] = *b"SLI-RES!";
    const RESPONSE_HEADER: [u8; 8] = *b"SRS-HEAD";

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<[u8], [u8]>()
        .request_user_header::<[u8; 8]>()
        .response_user_header::<[u8; 8]>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let client = service
        .client_builder()
        .initial_max_slice_len(REQUEST.len())
        .create()
        .unwrap();
    let server = service
        .server_builder()
        .initial_max_slice_len(RESPONSE.len())
        .create()
        .unwrap();

    let mut request = client.loan_slice(REQUEST.len()).unwrap();
    request.copy_from_slice(&REQUEST);
    *request.user_header_mut() = REQUEST_HEADER;
    let pending = request.send().unwrap();

    let active_request = server.receive().unwrap().expect("slice request");
    assert_eq!(&*active_request, &REQUEST);
    assert_eq!(*active_request.user_header(), REQUEST_HEADER);

    let mut response = active_request.loan_slice(RESPONSE.len()).unwrap();
    response.copy_from_slice(&RESPONSE);
    *response.user_header_mut() = RESPONSE_HEADER;
    response.send().unwrap();

    let response = pending.receive().unwrap().expect("slice response");
    assert_eq!(&*response, &RESPONSE);
    assert_eq!(*response.user_header(), RESPONSE_HEADER);
}

#[test]
fn one_hundred_client_slot_reuses_never_deliver_stale_payloads() {
    const CYCLES: u64 = 100;

    let node = create_node();
    let service = node
        .service_builder(&service_name())
        .request_response::<u64, u64>()
        .max_clients(1)
        .open_or_create()
        .unwrap();
    let server = service.server_builder().create().unwrap();
    let mut client = service.client_builder().create().unwrap();

    for cycle in 0..CYCLES {
        let old_pending = client.send_copy(cycle).unwrap();
        let old_request = server.receive().unwrap().expect("old request");
        let old_response = old_request
            .loan_uninit()
            .unwrap()
            .write_payload(u64::MAX - cycle);
        drop(old_pending);
        drop(client);

        client = service.client_builder().create().unwrap();
        let fresh_pending = client.send_copy(cycle + CYCLES).unwrap();
        let fresh_request = server.receive().unwrap().expect("fresh request");
        assert_eq!(*fresh_request, cycle + CYCLES);

        let _stale_send = old_response.send();
        assert!(fresh_pending.receive().unwrap().is_none(), "cycle {cycle}");

        fresh_request.send_copy(cycle + 2 * CYCLES).unwrap();
        let response = fresh_pending.receive().unwrap().expect("fresh response");
        assert_eq!(*response, cycle + 2 * CYCLES, "cycle {cycle}");

        drop(old_request);
    }
}

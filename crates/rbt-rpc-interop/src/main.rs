//! Fixed and dynamic-message cross-language RPC interop probe.
//!
//! This binary deliberately uses the canonical generated `Frame` request and
//! `Detection` response directly in native iceoryx2 loans.  The process role is
//! selected on the command line so the same executable can be used on either
//! side of a Rust/C++ or Rust/Python process pair.

use std::{
    env, process, thread,
    time::{Duration, Instant},
};

use rbt_iox2::{
    Detection, Frame, Image, PixelFormat, RpcClient, RpcDescriptor, RpcError, RpcResourceConfig,
    RpcServer, SliceRpcClient, SliceRpcServer,
};

const DEFAULT_CALLS: usize = 4;
const MAX_CALLS: usize = 1_000_000;
const BURST_WINDOW: usize = 16;
const CALL_TIMEOUT: Duration = Duration::from_secs(3);
const CLIENT_OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const NONCE_BASE: u64 = 0xBADC_0000_0000_0000;
const MARKER_BASE: u64 = 0xC0DE_0000;
const OPERAND_BASE: u64 = 0x0100;
const OPERAND_STEP: u64 = 17;
const TRANSFORM_MULTIPLIER: u64 = 0x9E37_79B1_85EB_CA87;
const TRANSFORM_XOR: u64 = 0xD1CE_F00D_CAFE_BEEF;
const DYNAMIC_REQUEST_BYTES: usize = 4_096;
const DYNAMIC_RESPONSE_BYTES: usize = 65_536;
const DYNAMIC_TIMESTAMP_BASE: u64 = 0x1020_3040_5060_7000;

fn quiet() -> bool {
    env::var("RBT_RPC_INTEROP_QUIET").is_ok_and(|value| value == "1")
}

fn report_index(index: usize) -> bool {
    !quiet() || index.is_multiple_of(100)
}

fn client_pace() {
    let milliseconds = env::var("RBT_RPC_INTEROP_PACE_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0);
    if milliseconds != 0 {
        thread::sleep(Duration::from_millis(milliseconds));
    }
}

/// Spreads `calls` evenly over `RBT_RPC_INTEROP_TARGET_MS` when set, so a
/// long-lived lane lasts roughly the target regardless of per-call cost.  Call
/// `index` is not followed by the next call before its share of the target
/// has elapsed; slow hosts simply run behind schedule without extra sleeps.
fn client_target_pace(started: Instant, index: usize, calls: usize) {
    let Some(target_ms) = env::var("RBT_RPC_INTEROP_TARGET_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .filter(|value| *value != 0)
    else {
        return;
    };
    let due_ms = target_ms.saturating_mul(index as u64 + 1) / calls.max(1) as u64;
    let due = started + Duration::from_millis(due_ms);
    let now = Instant::now();
    if due > now {
        thread::sleep(due - now);
    }
}

fn usage() -> ! {
    eprintln!(
        "usage: rbt-rpc-interop <client|server|burst-client|burst-server|dynamic-client|dynamic-server> <service-name> [calls]\n\
         calls must be between 1 and {MAX_CALLS} (default {DEFAULT_CALLS})"
    );
    process::exit(2);
}

fn parse_args() -> Result<(String, String, usize), String> {
    let mut args = env::args().skip(1);
    let role = args.next().unwrap_or_else(|| usage());
    if !matches!(
        role.as_str(),
        "client" | "server" | "burst-client" | "burst-server" | "dynamic-client" | "dynamic-server"
    ) {
        return Err(format!(
            "unsupported role {role:?}; expected client, server, dynamic-client, or dynamic-server"
        ));
    }
    let service = args.next().unwrap_or_else(|| usage());
    if service.is_empty() {
        return Err("service name must not be empty".to_owned());
    }
    let calls = match args.next() {
        Some(value) => value
            .parse::<usize>()
            .map_err(|error| format!("invalid calls value {value:?}: {error}"))?,
        None => DEFAULT_CALLS,
    };
    if args.next().is_some() {
        usage();
    }
    if calls == 0 || calls > MAX_CALLS {
        return Err(format!(
            "calls must be between 1 and {MAX_CALLS}, got {calls}"
        ));
    }
    Ok((role, service, calls))
}

fn resources() -> RpcResourceConfig {
    RpcResourceConfig::default()
        .with_max_in_flight(1)
        .with_max_clients(1)
        .with_max_request_loans(1)
        .with_response_buffer_size(1)
        .with_max_borrowed_responses(1)
        .with_max_response_loans(1)
        .with_max_request_slice_len(1)
        .with_max_response_slice_len(1)
}

fn descriptor(service: &str) -> RpcDescriptor {
    RpcDescriptor::for_fixed_with_resources::<Frame, Detection>(service, resources())
}

fn burst_resources() -> RpcResourceConfig {
    RpcResourceConfig::default()
        .with_max_in_flight(BURST_WINDOW)
        .with_max_clients(1)
        .with_max_request_loans(BURST_WINDOW)
        .with_response_buffer_size(1)
        .with_max_borrowed_responses(1)
        .with_max_response_loans(1)
        .with_max_request_slice_len(1)
        .with_max_response_slice_len(1)
}

fn burst_descriptor(service: &str) -> RpcDescriptor {
    RpcDescriptor::for_fixed_with_resources::<Frame, Detection>(service, burst_resources())
}

fn dynamic_resources() -> RpcResourceConfig {
    resources()
        .with_max_request_slice_len(DYNAMIC_REQUEST_BYTES)
        .with_max_response_slice_len(DYNAMIC_RESPONSE_BYTES)
}

fn dynamic_descriptor(service: &str) -> RpcDescriptor {
    RpcDescriptor::for_slice_with_resources::<Image, Image>(service, dynamic_resources())
}

fn report_idle(
    role: &str,
    service: &str,
    calls: usize,
    client_side: bool,
    pressure: rbt_iox2::RpcPressure,
) -> Result<(), String> {
    if pressure.service_name != service {
        return Err(format!(
            "idle snapshot service mismatch expected={service} actual={}",
            pressure.service_name
        ));
    }
    if pressure.snapshot_timestamp_unix_ms == 0 {
        return Err("idle snapshot timestamp must be positive".to_owned());
    }

    let expected_calls = calls as u64;
    if client_side {
        if !pressure.is_client() {
            return Err("idle snapshot is missing client metrics or has server metrics".to_owned());
        }
        let metrics = pressure
            .client
            .ok_or_else(|| "idle snapshot is missing client metrics".to_owned())?;
        println!(
            "RBT_RPC_INTEROP IDLE language=rust role={role} service={} calls={calls} endpoint=client snapshot_timestamp_unix_ms={} calls_started={} calls_completed={} in_flight_current={} response_guards_current={}",
            pressure.service_name,
            pressure.snapshot_timestamp_unix_ms,
            metrics.calls_started,
            metrics.calls_completed,
            metrics.in_flight_current,
            metrics.response_guards_current,
        );
        if metrics.calls_started != expected_calls
            || metrics.calls_completed != expected_calls
            || metrics.in_flight_current != 0
            || metrics.response_guards_current != 0
        {
            return Err(format!(
                "idle client counters mismatch expected_calls={expected_calls} started={} completed={} in_flight_current={} response_guards_current={}",
                metrics.calls_started,
                metrics.calls_completed,
                metrics.in_flight_current,
                metrics.response_guards_current,
            ));
        }
    } else {
        if !pressure.is_server() {
            return Err("idle snapshot is missing server metrics or has client metrics".to_owned());
        }
        let metrics = pressure
            .server
            .ok_or_else(|| "idle snapshot is missing server metrics".to_owned())?;
        println!(
            "RBT_RPC_INTEROP IDLE language=rust role={role} service={} calls={calls} endpoint=server snapshot_timestamp_unix_ms={} requests_received={} responses_sent={} requests_current={}",
            pressure.service_name,
            pressure.snapshot_timestamp_unix_ms,
            metrics.requests_received,
            metrics.responses_sent,
            metrics.requests_current,
        );
        if metrics.requests_received != expected_calls
            || metrics.responses_sent != expected_calls
            || metrics.requests_current != 0
        {
            return Err(format!(
                "idle server counters mismatch expected_calls={expected_calls} requests_received={} responses_sent={} requests_current={}",
                metrics.requests_received, metrics.responses_sent, metrics.requests_current,
            ));
        }
    }
    Ok(())
}

fn dynamic_nonce(index: usize) -> u64 {
    NONCE_BASE | index as u64
}

fn dynamic_request_byte(index: usize, offset: usize) -> u8 {
    ((index as u64)
        .wrapping_mul(17)
        .wrapping_add((offset as u64).wrapping_mul(31))
        .wrapping_add(7)
        & 0xff) as u8
}

fn dynamic_response_byte(index: usize, offset: usize) -> u8 {
    ((index as u64)
        .wrapping_mul(29)
        .wrapping_add((offset as u64).wrapping_mul(13))
        .wrapping_add(0xa5)
        & 0xff) as u8
}

fn dynamic_checksum(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .fold(14_695_981_039_346_656_037_u64, |checksum, byte| {
            (checksum ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        })
}

fn request_for(index: usize) -> Frame {
    let index = index as u64;
    let marker = MARKER_BASE | index;
    let operand = OPERAND_BASE + index * OPERAND_STEP;
    Frame {
        sequence: NONCE_BASE | index,
        value: (marker << 32) | operand,
    }
}

fn expected_value(nonce: u64, value: u64) -> u64 {
    nonce.wrapping_mul(TRANSFORM_MULTIPLIER) ^ value.rotate_left(23) ^ TRANSFORM_XOR
}

fn validate_request(index: usize, request: Frame) -> Result<(), String> {
    let expected = request_for(index);
    if request.sequence != expected.sequence || request.value != expected.value {
        return Err(format!(
            "request mismatch index={index} expected_nonce={} actual_nonce={} expected_value={} actual_value={}",
            expected.sequence, request.sequence, expected.value, request.value
        ));
    }
    Ok(())
}

fn run_server(service: &str, calls: usize) -> Result<(), String> {
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor(service))
        .map_err(|error| format!("open server: {error}"))?;
    println!("RBT_RPC_INTEROP READY language=rust role=server service={service} calls={calls}");

    for index in 0..calls {
        let request = server
            .receive_timeout(CALL_TIMEOUT)
            .map_err(|error| format!("receive index={index}: {error}"))?
            .ok_or_else(|| format!("receive index={index}: timed out"))?;
        let input = *request;
        validate_request(index, input)?;
        let marker = input.value >> 32;
        let operand = input.value & u64::from(u32::MAX);
        if report_index(index) {
            println!(
                "RBT_RPC_INTEROP REQUEST language=rust role=server index={index} nonce={} marker={marker} operand={operand}",
                input.sequence
            );
        }
        let mut response = request
            .loan_response()
            .map_err(|error| format!("loan response index={index}: {error}"))?;
        response.sequence = input.sequence;
        response.value = expected_value(input.sequence, input.value);
        response
            .send()
            .map_err(|error| format!("send response index={index}: {error}"))?;
    }

    report_idle("server", service, calls, false, server.pressure_snapshot())?;
    println!("RBT_RPC_INTEROP PASS language=rust role=server service={service} calls={calls}");
    drop(server);
    Ok(())
}

fn run_client(service: &str, calls: usize) -> Result<(), String> {
    // Keep a short retry window for the native service connection.  The
    // interop script starts clients only after the peer's READY marker, but a
    // bounded retry also makes direct probe use less timing-sensitive.
    let open_started = Instant::now();
    let client = loop {
        match RpcClient::<Frame, Detection>::open(&descriptor(service)) {
            Ok(client) => break client,
            Err(error @ RpcError::RequestResponseOpen(_))
            | Err(error @ RpcError::EventOpen(_))
            | Err(error @ RpcError::EventOpenOrCreate(_)) => {
                if open_started.elapsed() >= CLIENT_OPEN_TIMEOUT {
                    return Err(format!(
                        "open client timed out after {:?}: {error}",
                        CLIENT_OPEN_TIMEOUT
                    ));
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(error) => return Err(format!("open client: {error}")),
        }
    };
    println!("RBT_RPC_INTEROP READY language=rust role=client service={service} calls={calls}");

    let started = Instant::now();
    for index in 0..calls {
        let input = request_for(index);
        let response = client
            .call_copy(input, CALL_TIMEOUT)
            .map_err(|error| format!("call index={index}: {error}"))?;
        let expected = expected_value(input.sequence, input.value);
        if response.sequence != input.sequence || response.value != expected {
            return Err(format!(
                "response mismatch index={index} expected_nonce={} actual_nonce={} expected_value={} actual_value={}",
                input.sequence, response.sequence, expected, response.value
            ));
        }
        if report_index(index) {
            println!(
                "RBT_RPC_INTEROP RESPONSE language=rust role=client index={index} nonce={} checksum={}",
                response.sequence, response.value
            );
        }
        client_pace();
        client_target_pace(started, index, calls);
    }

    report_idle("client", service, calls, true, client.pressure_snapshot())?;
    println!("RBT_RPC_INTEROP PASS language=rust role=client service={service} calls={calls}");
    drop(client);
    Ok(())
}

fn run_burst_server(service: &str, calls: usize) -> Result<(), String> {
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&burst_descriptor(service))
        .map_err(|error| format!("open burst server: {error}"))?;
    println!(
        "RBT_RPC_INTEROP READY language=rust role=burst-server service={service} calls={calls}"
    );

    let mut base = 0;
    while base < calls {
        let window_len = BURST_WINDOW.min(calls - base);
        let mut requests = Vec::with_capacity(window_len);
        for offset in 0..window_len {
            let index = base + offset;
            let request = server
                .receive_timeout(CALL_TIMEOUT)
                .map_err(|error| format!("receive burst index={index}: {error}"))?
                .ok_or_else(|| format!("receive burst index={index}: timed out"))?;
            let input = *request;
            validate_request(index, input)?;
            if report_index(index) {
                println!(
                    "RBT_RPC_INTEROP REQUEST language=rust role=burst-server index={index} nonce={} value={}",
                    input.sequence, input.value
                );
            }
            requests.push(request);
        }

        for offset in (0..window_len).rev() {
            let index = base + offset;
            let request = requests.pop().expect("received full burst window");
            let input = *request;
            let mut response = request
                .loan_response()
                .map_err(|error| format!("loan burst response index={index}: {error}"))?;
            response.sequence = input.sequence;
            response.value = expected_value(input.sequence, input.value);
            response
                .send()
                .map_err(|error| format!("send burst response index={index}: {error}"))?;
        }
        base += window_len;
    }

    report_idle(
        "burst-server",
        service,
        calls,
        false,
        server.pressure_snapshot(),
    )?;
    println!(
        "RBT_RPC_INTEROP PASS language=rust role=burst-server service={service} calls={calls}"
    );
    drop(server);
    Ok(())
}

fn run_burst_client(service: &str, calls: usize) -> Result<(), String> {
    let open_started = Instant::now();
    let client = loop {
        match RpcClient::<Frame, Detection>::open(&burst_descriptor(service)) {
            Ok(client) => break client,
            Err(error @ RpcError::RequestResponseOpen(_))
            | Err(error @ RpcError::EventOpen(_))
            | Err(error @ RpcError::EventOpenOrCreate(_)) => {
                if open_started.elapsed() >= CLIENT_OPEN_TIMEOUT {
                    return Err(format!(
                        "open burst client timed out after {:?}: {error}",
                        CLIENT_OPEN_TIMEOUT
                    ));
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(error) => return Err(format!("open burst client: {error}")),
        }
    };
    println!(
        "RBT_RPC_INTEROP READY language=rust role=burst-client service={service} calls={calls}"
    );

    let mut base = 0;
    while base < calls {
        let window_len = BURST_WINDOW.min(calls - base);
        let mut pending_calls = Vec::with_capacity(window_len);
        for offset in 0..window_len {
            let index = base + offset;
            let input = request_for(index);
            let mut request = client
                .loan_request()
                .map_err(|error| format!("loan burst request index={index}: {error}"))?;
            *request = input;
            pending_calls.push((
                index,
                input,
                request
                    .send()
                    .map_err(|error| format!("send burst request index={index}: {error}"))?,
            ));
        }

        for (index, input, pending) in pending_calls.into_iter().rev() {
            let response = pending
                .wait(CALL_TIMEOUT)
                .map_err(|error| format!("wait burst response index={index}: {error}"))?;
            let expected = expected_value(input.sequence, input.value);
            if response.sequence != input.sequence || response.value != expected {
                return Err(format!(
                    "burst response mismatch index={index} expected_nonce={} actual_nonce={} expected_value={} actual_value={}",
                    input.sequence, response.sequence, expected, response.value
                ));
            }
            if report_index(index) {
                println!(
                    "RBT_RPC_INTEROP RESPONSE language=rust role=burst-client index={index} nonce={} checksum={}",
                    response.sequence, response.value
                );
            }
        }
        base += window_len;
    }

    report_idle(
        "burst-client",
        service,
        calls,
        true,
        client.pressure_snapshot(),
    )?;
    println!(
        "RBT_RPC_INTEROP PASS language=rust role=burst-client service={service} calls={calls}"
    );
    drop(client);
    Ok(())
}

fn run_dynamic_server(service: &str, calls: usize) -> Result<(), String> {
    let mut server = SliceRpcServer::<Image, Image>::open_or_create(&dynamic_descriptor(service))
        .map_err(|error| format!("open dynamic server: {error}"))?;
    println!(
        "RBT_RPC_INTEROP READY language=rust role=dynamic-server service={service} calls={calls}"
    );
    for index in 0..calls {
        let request = server
            .receive_timeout(CALL_TIMEOUT)
            .map_err(|error| format!("receive dynamic index={index}: {error}"))?
            .ok_or_else(|| format!("receive dynamic index={index}: timed out"))?;
        let header = *request.user_header();
        if header.sequence != dynamic_nonce(index)
            || header.timestamp_ns != DYNAMIC_TIMESTAMP_BASE.wrapping_add(index as u64)
            || header.width as usize != DYNAMIC_REQUEST_BYTES
            || header.height != 1
            || header.stride as usize != DYNAMIC_REQUEST_BYTES
            || header.encoding != PixelFormat::GRAY8
            || request.len() != DYNAMIC_REQUEST_BYTES
        {
            return Err(format!("dynamic request header mismatch index={index}"));
        }
        for (offset, actual) in request.iter().copied().enumerate() {
            let expected = dynamic_request_byte(index, offset);
            if actual != expected {
                return Err(format!(
                    "dynamic request byte mismatch index={index} offset={offset} expected={expected} actual={actual}"
                ));
            }
        }
        let request_checksum = dynamic_checksum(&request);
        let mut response = request
            .loan_response_slice(DYNAMIC_RESPONSE_BYTES)
            .map_err(|error| format!("loan dynamic response index={index}: {error}"))?;
        {
            let output_header = response.user_header_mut();
            output_header.sequence = header.sequence;
            output_header.timestamp_ns = header.timestamp_ns ^ TRANSFORM_XOR;
            output_header.width = DYNAMIC_RESPONSE_BYTES as u32;
            output_header.height = 1;
            output_header.stride = DYNAMIC_RESPONSE_BYTES as u32;
            output_header.encoding = PixelFormat::GRAY8;
        }
        for (offset, byte) in response.iter_mut().enumerate() {
            *byte = dynamic_response_byte(index, offset);
        }
        let response_checksum = dynamic_checksum(&response);
        response
            .send()
            .map_err(|error| format!("send dynamic response index={index}: {error}"))?;
        if report_index(index) {
            println!(
                "RBT_RPC_INTEROP DYNAMIC language=rust role=server index={index} nonce={} request_bytes={} request_checksum={request_checksum} response_bytes={} response_checksum={response_checksum}",
                header.sequence, DYNAMIC_REQUEST_BYTES, DYNAMIC_RESPONSE_BYTES,
            );
        }
    }
    report_idle(
        "dynamic-server",
        service,
        calls,
        false,
        server.pressure_snapshot(),
    )?;
    println!(
        "RBT_RPC_INTEROP PASS language=rust role=dynamic-server service={service} calls={calls}"
    );
    Ok(())
}

fn run_dynamic_client(service: &str, calls: usize) -> Result<(), String> {
    let open_started = Instant::now();
    let client = loop {
        match SliceRpcClient::<Image, Image>::open(&dynamic_descriptor(service)) {
            Ok(client) => break client,
            Err(error @ RpcError::RequestResponseOpen(_))
            | Err(error @ RpcError::EventOpen(_))
            | Err(error @ RpcError::EventOpenOrCreate(_)) => {
                if open_started.elapsed() >= CLIENT_OPEN_TIMEOUT {
                    return Err(format!("open dynamic client timed out: {error}"));
                }
                thread::sleep(Duration::from_millis(25));
            }
            Err(error) => return Err(format!("open dynamic client: {error}")),
        }
    };
    println!(
        "RBT_RPC_INTEROP READY language=rust role=dynamic-client service={service} calls={calls}"
    );
    for index in 0..calls {
        let mut request = client
            .loan_request_slice(DYNAMIC_REQUEST_BYTES)
            .map_err(|error| format!("loan dynamic request index={index}: {error}"))?;
        {
            let header = request.user_header_mut();
            header.sequence = dynamic_nonce(index);
            header.timestamp_ns = DYNAMIC_TIMESTAMP_BASE.wrapping_add(index as u64);
            header.width = DYNAMIC_REQUEST_BYTES as u32;
            header.height = 1;
            header.stride = DYNAMIC_REQUEST_BYTES as u32;
            header.encoding = PixelFormat::GRAY8;
        }
        for (offset, byte) in request.iter_mut().enumerate() {
            *byte = dynamic_request_byte(index, offset);
        }
        let request_checksum = dynamic_checksum(&request);
        let response = request
            .send()
            .map_err(|error| format!("send dynamic request index={index}: {error}"))?
            .wait(CALL_TIMEOUT)
            .map_err(|error| format!("wait dynamic response index={index}: {error}"))?;
        let header = *response.user_header();
        if header.sequence != dynamic_nonce(index)
            || header.timestamp_ns
                != DYNAMIC_TIMESTAMP_BASE.wrapping_add(index as u64) ^ TRANSFORM_XOR
            || header.width as usize != DYNAMIC_RESPONSE_BYTES
            || header.height != 1
            || header.stride as usize != DYNAMIC_RESPONSE_BYTES
            || header.encoding != PixelFormat::GRAY8
            || response.len() != DYNAMIC_RESPONSE_BYTES
        {
            return Err(format!("dynamic response header mismatch index={index}"));
        }
        for (offset, actual) in response.iter().copied().enumerate() {
            let expected = dynamic_response_byte(index, offset);
            if actual != expected {
                return Err(format!(
                    "dynamic response byte mismatch index={index} offset={offset} expected={expected} actual={actual}"
                ));
            }
        }
        if report_index(index) {
            println!(
                "RBT_RPC_INTEROP DYNAMIC language=rust role=client index={index} nonce={} request_bytes={} request_checksum={request_checksum} response_bytes={} response_checksum={}",
                header.sequence,
                DYNAMIC_REQUEST_BYTES,
                DYNAMIC_RESPONSE_BYTES,
                dynamic_checksum(&response),
            );
        }
        client_pace();
    }
    report_idle(
        "dynamic-client",
        service,
        calls,
        true,
        client.pressure_snapshot(),
    )?;
    println!(
        "RBT_RPC_INTEROP PASS language=rust role=dynamic-client service={service} calls={calls}"
    );
    Ok(())
}

fn run() -> Result<(), String> {
    let (role, service, calls) = parse_args()?;
    match role.as_str() {
        "client" => run_client(&service, calls),
        "server" => run_server(&service, calls),
        "burst-client" => run_burst_client(&service, calls),
        "burst-server" => run_burst_server(&service, calls),
        "dynamic-client" => run_dynamic_client(&service, calls),
        "dynamic-server" => run_dynamic_server(&service, calls),
        _ => unreachable!("role validated by parse_args"),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("RBT_RPC_INTEROP FAIL language=rust error={error}");
        process::exit(1);
    }
}

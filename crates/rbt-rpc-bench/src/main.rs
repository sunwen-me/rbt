use std::{
    error::Error,
    io::{self, Write},
    process,
    time::Instant,
};

use rbt_bench::{ProcessStats, cpu_seconds_between, process_stats};
use rbt_iox2::{
    Detection, Frame, Image, ImageHeader, PixelFormat, RpcClient, RpcDescriptor, RpcResourceConfig,
    RpcServer, SliceRpcClient, SliceRpcServer,
};
use rbt_rpc_bench::{
    Case, Config, ParseOutcome, Role, checksum, nonce_for, pack_result_checksum, payload_byte,
    percentile, result_for, unpack_result_checksum, value_for,
};

type BenchResult<T> = Result<T, Box<dyn Error + Send + Sync>>;
type DynamicClient = SliceRpcClient<Image, Image>;
type DynamicServer = SliceRpcServer<Image, Image>;

fn main() {
    let parsed = match rbt_rpc_bench::parse_args(std::env::args().skip(1)) {
        Ok(parsed) => parsed,
        Err(error) => {
            eprintln!("RBT_RPC_BENCH_ERROR error={}", machine_value(&error));
            eprintln!("{}", rbt_rpc_bench::usage("rbt-rpc-bench"));
            process::exit(2);
        }
    };

    match parsed {
        ParseOutcome::Help => {
            println!("{}", rbt_rpc_bench::usage("rbt-rpc-bench"));
        }
        ParseOutcome::Capabilities => print_capabilities(),
        ParseOutcome::Run(config) => {
            let result = match config.role {
                Role::Server => run_server(&config),
                Role::Client => run_client(&config),
            };
            if let Err(error) = result {
                eprintln!(
                    "RBT_RPC_BENCH_ERROR role={} case={} service={} error={}",
                    config.role,
                    config.case,
                    machine_value(&config.service),
                    machine_value(&error.to_string()),
                );
                process::exit(1);
            }
        }
    }
}

fn print_capabilities() {
    println!(
        "RBT_RPC_BENCH_CAPABILITY native_rpc=available dynamic_image=available dynamic_20m=available"
    );
}

fn run_server(config: &Config) -> BenchResult<()> {
    match config.case {
        Case::Frame16 => run_frame_server(config),
        Case::Image1K1K | Case::Image1K1M | Case::Image1K20M => run_image_server(config),
    }
}

fn run_client(config: &Config) -> BenchResult<()> {
    match config.case {
        Case::Frame16 => run_frame_client(config),
        Case::Image1K1K | Case::Image1K1M | Case::Image1K20M => run_image_client(config),
    }
}

fn frame_descriptor(config: &Config) -> RpcDescriptor {
    RpcDescriptor::for_fixed_with_resources::<Frame, Detection>(
        config.service.clone(),
        RpcResourceConfig::new(1, 1, 1, 1, 1, 1, 1, 1),
    )
}

fn image_descriptor(config: &Config) -> RpcDescriptor {
    RpcDescriptor::for_slice_with_resources::<Image, Image>(
        config.service.clone(),
        RpcResourceConfig::new(
            1,
            1,
            1,
            1,
            1,
            1,
            config.case.request_bytes(),
            config.case.response_bytes(),
        ),
    )
}

fn run_frame_server(config: &Config) -> BenchResult<()> {
    let descriptor = frame_descriptor(config);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    print_ready(config);
    let start_stats = process_stats();
    let started = Instant::now();
    let total = config.total_calls()?;
    let mut measured_started = None;
    for ordinal in 0..total {
        if ordinal == config.warmup {
            measured_started = Some(Instant::now());
        }
        let request = server
            .receive_timeout(config.timeout)?
            .ok_or_else(|| timeout_error("server receive", ordinal))?;
        let nonce = request.sequence;
        let (value, request_checksum) = unpack_result_checksum(request.value);
        let expected_request_checksum = checksum(nonce, value, &[]);
        if request_checksum != expected_request_checksum {
            return Err(validation_error(
                "request checksum",
                ordinal,
                u64::from(expected_request_checksum),
                u64::from(request_checksum),
            ));
        }
        let result = result_for(nonce, value);
        let response_checksum = checksum(nonce, result, &[]);
        let mut response = request.loan_response()?;
        response.sequence = nonce;
        response.value = pack_result_checksum(result, response_checksum);
        response.send()?;
    }
    let elapsed_ns = elapsed_nanos(measured_started.unwrap_or(started));
    let total_elapsed_ns = elapsed_nanos(started);
    let end_stats = process_stats();
    print_server_result(
        config,
        elapsed_ns,
        total_elapsed_ns,
        start_stats,
        end_stats,
        total,
    );
    Ok(())
}

fn run_frame_client(config: &Config) -> BenchResult<()> {
    let descriptor = frame_descriptor(config);
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    print_ready(config);
    let start_stats = process_stats();
    let started = Instant::now();
    let total = config.total_calls()?;
    let mut measured_started = None;
    let mut round_trips = Vec::with_capacity(usize::try_from(config.count).unwrap_or(usize::MAX));
    for ordinal in 0..total {
        if ordinal == config.warmup {
            measured_started = Some(Instant::now());
        }
        let nonce = nonce_for(ordinal);
        let value = value_for(nonce);
        let request_checksum = checksum(nonce, value, &[]);
        let request = Frame {
            sequence: nonce,
            value: pack_result_checksum(value, request_checksum),
        };
        let call_started = Instant::now();
        let response = client.call_copy(request, config.timeout)?;
        let round_trip_ns = elapsed_nanos(call_started);
        validate_frame_response(&response, nonce, value, ordinal)?;
        if ordinal >= config.warmup {
            round_trips.push(round_trip_ns);
        }
    }
    let elapsed_ns = elapsed_nanos(measured_started.unwrap_or(started));
    let total_elapsed_ns = elapsed_nanos(started);
    let end_stats = process_stats();
    print_client_result(
        config,
        elapsed_ns,
        total_elapsed_ns,
        start_stats,
        end_stats,
        &round_trips,
    )?;
    Ok(())
}

fn validate_frame_response(
    response: &Detection,
    nonce: u64,
    value: u32,
    ordinal: u64,
) -> BenchResult<()> {
    if response.sequence != nonce {
        return Err(validation_error(
            "response nonce",
            ordinal,
            nonce,
            response.sequence,
        ));
    }
    let (result, response_checksum) = unpack_result_checksum(response.value);
    let expected_result = result_for(nonce, value);
    if result != expected_result {
        return Err(validation_error(
            "response result",
            ordinal,
            u64::from(expected_result),
            u64::from(result),
        ));
    }
    let expected_checksum = checksum(nonce, result, &[]);
    if response_checksum != expected_checksum {
        return Err(validation_error(
            "response checksum",
            ordinal,
            u64::from(expected_checksum),
            u64::from(response_checksum),
        ));
    }
    Ok(())
}

fn run_image_server(config: &Config) -> BenchResult<()> {
    let descriptor = image_descriptor(config);
    let mut server = DynamicServer::open_or_create(&descriptor)?;
    print_ready(config);
    let start_stats = process_stats();
    let started = Instant::now();
    let total = config.total_calls()?;
    let mut measured_started = None;
    for ordinal in 0..total {
        if ordinal == config.warmup {
            measured_started = Some(Instant::now());
        }
        let request = server
            .receive_timeout(config.timeout)?
            .ok_or_else(|| timeout_error("server receive", ordinal))?;
        let request_header = *request.user_header();
        let request_payload = &*request;
        let nonce = request_header.sequence;
        let value = u32::try_from(request_header.timestamp_ns)
            .map_err(|_| validation_text("request value exceeds u32", ordinal))?;
        let expected_request_checksum = checksum(nonce, value, request_payload);
        validate_image_header(
            request_header,
            config.case.request_bytes(),
            expected_request_checksum,
            value,
            ordinal,
        )?;
        validate_payload(
            request_payload,
            nonce ^ u64::from(value),
            ordinal,
            "request",
        )?;

        let result = result_for(nonce, value);
        let response_len = config.case.response_bytes();
        let mut response = request.loan_response_slice(response_len)?;
        {
            let response_payload = &mut *response;
            fill_payload(response_payload, nonce ^ u64::from(result));
            let response_checksum = checksum(nonce, result, response_payload);
            *response.user_header_mut() =
                image_header(nonce, u64::from(result), response_checksum, response_len)?;
        }
        response.send()?;
    }
    let elapsed_ns = elapsed_nanos(measured_started.unwrap_or(started));
    let total_elapsed_ns = elapsed_nanos(started);
    let end_stats = process_stats();
    print_server_result(
        config,
        elapsed_ns,
        total_elapsed_ns,
        start_stats,
        end_stats,
        total,
    );
    Ok(())
}

fn run_image_client(config: &Config) -> BenchResult<()> {
    let descriptor = image_descriptor(config);
    let client = DynamicClient::open(&descriptor)?;
    print_ready(config);
    let start_stats = process_stats();
    let started = Instant::now();
    let total = config.total_calls()?;
    let mut measured_started = None;
    let mut round_trips = Vec::with_capacity(usize::try_from(config.count).unwrap_or(usize::MAX));
    for ordinal in 0..total {
        if ordinal == config.warmup {
            measured_started = Some(Instant::now());
        }
        let nonce = nonce_for(ordinal);
        let value = value_for(nonce);
        let request_len = config.case.request_bytes();
        let mut request = client.loan_request_slice(request_len)?;
        {
            let request_payload = &mut *request;
            fill_payload(request_payload, nonce ^ u64::from(value));
            let request_checksum = checksum(nonce, value, request_payload);
            *request.user_header_mut() =
                image_header(nonce, u64::from(value), request_checksum, request_len)?;
        }
        let call_started = Instant::now();
        let pending = request.send()?;
        let response = pending.wait(config.timeout)?;
        let round_trip_ns = elapsed_nanos(call_started);
        let response_header = *response.user_header();
        let response_payload = &*response;
        let result = u32::try_from(response_header.timestamp_ns)
            .map_err(|_| validation_text("response result exceeds u32", ordinal))?;
        let expected_result = result_for(nonce, value);
        if response_header.sequence != nonce {
            return Err(validation_error(
                "response nonce",
                ordinal,
                nonce,
                response_header.sequence,
            ));
        }
        if result != expected_result {
            return Err(validation_error(
                "response result",
                ordinal,
                u64::from(expected_result),
                u64::from(result),
            ));
        }
        if response_payload.len() != config.case.response_bytes() {
            return Err(validation_error(
                "response length",
                ordinal,
                config.case.response_bytes() as u64,
                response_payload.len() as u64,
            ));
        }
        validate_payload(
            response_payload,
            nonce ^ u64::from(result),
            ordinal,
            "response",
        )?;
        let expected_checksum = checksum(nonce, result, response_payload);
        if response_header.width != expected_checksum {
            return Err(validation_error(
                "response checksum",
                ordinal,
                u64::from(expected_checksum),
                u64::from(response_header.width),
            ));
        }
        if ordinal >= config.warmup {
            round_trips.push(round_trip_ns);
        }
    }
    let elapsed_ns = elapsed_nanos(measured_started.unwrap_or(started));
    let total_elapsed_ns = elapsed_nanos(started);
    let end_stats = process_stats();
    print_client_result(
        config,
        elapsed_ns,
        total_elapsed_ns,
        start_stats,
        end_stats,
        &round_trips,
    )?;
    Ok(())
}

fn image_header(
    nonce: u64,
    scalar: u64,
    checksum: u32,
    payload_len: usize,
) -> BenchResult<ImageHeader> {
    let payload_len = u32::try_from(payload_len)
        .map_err(|_| format!("payload length {payload_len} exceeds ImageHeader stride"))?;
    Ok(ImageHeader {
        sequence: nonce,
        timestamp_ns: scalar,
        width: checksum,
        height: 0,
        stride: payload_len,
        encoding: PixelFormat::GRAY8,
    })
}

fn validate_image_header(
    header: ImageHeader,
    expected_length: usize,
    expected_checksum: u32,
    expected_value: u32,
    ordinal: u64,
) -> BenchResult<()> {
    if header.stride as usize != expected_length {
        return Err(validation_error(
            "request length",
            ordinal,
            expected_length as u64,
            u64::from(header.stride),
        ));
    }
    if header.timestamp_ns != u64::from(expected_value) {
        return Err(validation_error(
            "request value",
            ordinal,
            u64::from(expected_value),
            header.timestamp_ns,
        ));
    }
    if header.width != expected_checksum {
        return Err(validation_error(
            "request checksum",
            ordinal,
            u64::from(expected_checksum),
            u64::from(header.width),
        ));
    }
    if header.height != 0 || header.encoding != PixelFormat::GRAY8 {
        return Err(validation_text("request header metadata mismatch", ordinal));
    }
    Ok(())
}

fn fill_payload(payload: &mut [u8], seed: u64) {
    for (index, byte) in payload.iter_mut().enumerate() {
        *byte = payload_byte(seed, index);
    }
}

fn validate_payload(payload: &[u8], seed: u64, ordinal: u64, direction: &str) -> BenchResult<()> {
    for (index, &actual) in payload.iter().enumerate() {
        let expected = payload_byte(seed, index);
        if actual != expected {
            return Err(validation_text(
                &format!(
                    "{direction} payload mismatch index={index} expected={expected} actual={actual}"
                ),
                ordinal,
            ));
        }
    }
    Ok(())
}

fn print_ready(config: &Config) {
    println!(
        "RBT_RPC_BENCH_READY role={} case={} service={} request_bytes={} response_bytes={} dynamic={}",
        config.role,
        config.case,
        machine_value(&config.service),
        config.case.request_bytes(),
        config.case.response_bytes(),
        config.case.is_dynamic(),
    );
    let _ = io::stdout().flush();
}

fn print_server_result(
    config: &Config,
    elapsed_ns: u64,
    total_elapsed_ns: u64,
    start_stats: ProcessStats,
    end_stats: ProcessStats,
    calls_received: u64,
) {
    println!(
        "RBT_RPC_BENCH_RESULT role=server case={} service={} request_bytes={} response_bytes={} warmup={} measured={} calls_received={} elapsed_ns={} total_elapsed_ns={} calls_per_s={} cpu_seconds={} rss_start_bytes={} rss_end_bytes={}",
        config.case,
        machine_value(&config.service),
        config.case.request_bytes(),
        config.case.response_bytes(),
        config.warmup,
        config.count,
        calls_received,
        elapsed_ns,
        total_elapsed_ns,
        calls_per_second(config.count, elapsed_ns),
        format_optional_f64(cpu_seconds_between(start_stats, end_stats)),
        format_optional_u64(start_stats.rss_bytes),
        format_optional_u64(end_stats.rss_bytes),
    );
    let _ = io::stdout().flush();
}

fn print_client_result(
    config: &Config,
    elapsed_ns: u64,
    total_elapsed_ns: u64,
    start_stats: ProcessStats,
    end_stats: ProcessStats,
    round_trips: &[u64],
) -> BenchResult<()> {
    if round_trips.len() != usize::try_from(config.count).unwrap_or(usize::MAX) {
        return Err(format!(
            "measured round-trip sample count mismatch: expected {}, got {}",
            config.count,
            round_trips.len()
        )
        .into());
    }
    let p50 = percentile(round_trips, 50.0).ok_or("p50 requires at least one sample")?;
    let p90 = percentile(round_trips, 90.0).ok_or("p90 requires at least one sample")?;
    let p99 = percentile(round_trips, 99.0).ok_or("p99 requires at least one sample")?;
    let max = round_trips
        .iter()
        .copied()
        .max()
        .ok_or("max requires a sample")?;
    println!(
        "RBT_RPC_BENCH_RESULT role=client case={} service={} request_bytes={} response_bytes={} warmup={} measured={} elapsed_ns={} total_elapsed_ns={} calls_per_s={} rtt_p50_ns={} rtt_p90_ns={} rtt_p99_ns={} rtt_max_ns={} responses_validated={} validation=nonce,result,checksum cpu_seconds={} rss_start_bytes={} rss_end_bytes={}",
        config.case,
        machine_value(&config.service),
        config.case.request_bytes(),
        config.case.response_bytes(),
        config.warmup,
        config.count,
        elapsed_ns,
        total_elapsed_ns,
        calls_per_second(config.count, elapsed_ns),
        p50,
        p90,
        p99,
        max,
        config.count + config.warmup,
        format_optional_f64(cpu_seconds_between(start_stats, end_stats)),
        format_optional_u64(start_stats.rss_bytes),
        format_optional_u64(end_stats.rss_bytes),
    );
    let _ = io::stdout().flush();
    Ok(())
}

fn calls_per_second(calls: u64, elapsed_ns: u64) -> String {
    if elapsed_ns == 0 {
        return "na".to_owned();
    }
    format!("{:.3}", calls as f64 * 1_000_000_000.0 / elapsed_ns as f64)
}

fn elapsed_nanos(started: Instant) -> u64 {
    u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX)
}

fn format_optional_u64(value: Option<u64>) -> String {
    value.map_or_else(|| "na".to_owned(), |value| value.to_string())
}

fn format_optional_f64(value: Option<f64>) -> String {
    value.map_or_else(|| "na".to_owned(), |value| format!("{value:.9}"))
}

fn timeout_error(operation: &str, ordinal: u64) -> Box<dyn Error + Send + Sync> {
    format!("{operation} timed out at ordinal {ordinal}").into()
}

fn validation_text(message: &str, ordinal: u64) -> Box<dyn Error + Send + Sync> {
    format!("validation failed at ordinal {ordinal}: {message}").into()
}

fn validation_error(
    field: &str,
    ordinal: u64,
    expected: u64,
    actual: u64,
) -> Box<dyn Error + Send + Sync> {
    validation_text(
        &format!("{field} expected={expected} actual={actual}"),
        ordinal,
    )
}

fn machine_value(value: &str) -> String {
    value
        .chars()
        .map(|character| match character {
            'a'..='z' | 'A'..='Z' | '0'..='9' | '_' | '-' | '/' | '.' | ':' => character,
            _ => '_',
        })
        .collect()
}

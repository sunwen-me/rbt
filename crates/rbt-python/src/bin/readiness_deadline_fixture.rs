use std::{
    env,
    error::Error,
    io::{self, Read, Write},
    time::Duration,
};

use iceoryx2::prelude::*;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn usage(program: &str) -> String {
    format!("usage: {program} SERVICE [DEADLINE_MS]")
}

fn main() -> Result<()> {
    let mut args = env::args();
    let program = args
        .next()
        .unwrap_or_else(|| "readiness_deadline_fixture".to_owned());
    let service = args.next().ok_or_else(|| usage(&program))?;
    let deadline_ms = args
        .next()
        .map(|value| {
            value
                .parse::<u64>()
                .map_err(|error| format!("invalid deadline_ms {value:?}: {error}"))
        })
        .transpose()?;
    if args.next().is_some() {
        return Err(usage(&program).into());
    }
    let deadline_ms = deadline_ms.unwrap_or(10);

    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let ready_service = format!("{service}/ready");
    let ready_service_name: ServiceName = ready_service.as_str().try_into()?;
    let event = node
        .service_builder(&ready_service_name)
        .event()
        .deadline(Duration::from_millis(deadline_ms))
        .open_or_create()?;
    let _notifier = event.notifier_builder().create()?;

    println!("READINESS_DEADLINE_READY service={service} deadline_ms={deadline_ms}");
    io::stdout().flush()?;

    let mut stdin = io::stdin().lock();
    let mut buffer = [0_u8; 1024];
    while stdin.read(&mut buffer)? != 0 {}

    Ok(())
}

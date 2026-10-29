#![forbid(unsafe_code)]

mod lifecycle;
mod sdk_transport;
mod transport;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(about = "Finite acceptance probes for the maintained iceoryx2 fork")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    Publish(transport::Options),
    Consume(transport::Options),
    SdkPublish {
        #[arg(long, default_value = sdk_transport::DEFAULT_SERVICE)]
        service: String,
    },
    SdkConsume {
        #[arg(long, default_value = sdk_transport::DEFAULT_SERVICE)]
        service: String,
    },
    RejectMismatch {
        #[arg(long)]
        service: String,
    },
    Lifecycle {
        #[arg(long)]
        service: String,
        #[arg(long, default_value_t = 60000)]
        timeout_ms: u64,
        #[arg(long, default_value_t = false)]
        expect_recovery: bool,
    },
    /// Reuse the original exact-layout upstream probe against the fork core.
    Upstream {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
        args: Vec<String>,
    },
}

fn main() -> anyhow::Result<()> {
    match Cli::parse().command {
        Command::Publish(options) => transport::publish(options),
        Command::Consume(options) => transport::consume(options),
        Command::SdkPublish { service } => sdk_transport::publish(&service),
        Command::SdkConsume { service } => sdk_transport::consume(&service),
        Command::RejectMismatch { service } => transport::reject_mismatch(&service),
        Command::Lifecycle {
            service,
            timeout_ms,
            expect_recovery,
        } => tokio::runtime::Runtime::new()?.block_on(lifecycle::run(
            service,
            timeout_ms,
            expect_recovery,
        )),
        Command::Upstream { args } => {
            let command = cross_host_upstream_probe::parse_args(args)
                .map_err(|error| anyhow::anyhow!(error.to_string()))?;
            cross_host_upstream_probe::run(command)
                .map_err(|error| anyhow::anyhow!(error.to_string()))
        }
    }
}

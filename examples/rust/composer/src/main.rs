use tracing_subscriber::EnvFilter;

fn init_tracing() {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        // Plain text when stdout is redirected to a log file or supervisor.
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stdout()))
        .init();
}

fn composer_addr() -> String {
    std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned())
}

#[tokio::main]
async fn main() -> Result<(), rbt_compose::ControlPlaneError> {
    let arguments = std::env::args().skip(1).collect::<Vec<_>>();
    if matches!(arguments.as_slice(), [argument] if argument == "--version" || argument == "version")
    {
        println!("composer {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }
    if !arguments.is_empty() {
        eprintln!("usage: composer [--version]");
        std::process::exit(2);
    }
    init_tracing();
    rbt_compose::serve(composer_addr()).await
}

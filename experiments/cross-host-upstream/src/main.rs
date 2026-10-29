#![forbid(unsafe_code)]

use cross_host_upstream_probe::{parse_args, run};

fn main() {
    match parse_args(std::env::args().skip(1)) {
        Ok(command) => {
            if let Err(error) = run(command) {
                eprintln!("ERROR {error}");
                std::process::exit(1);
            }
        }
        Err(error) if error.to_string() == "help requested" => {
            cross_host_upstream_probe::print_help();
        }
        Err(error) => {
            eprintln!("ERROR {error}; use --help");
            std::process::exit(2);
        }
    }
}

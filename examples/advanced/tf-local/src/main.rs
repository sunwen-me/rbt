use std::io::Read;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use rbt_core::CapabilityInstanceId;
use rbt_tf::{
    LookupPolicy, StaticExtrinsicsConfig, TfResourceConfig, TransformBuffer, Vec3,
    config::MAX_STATIC_CONFIG_BYTES,
};
use rbt_time::HostClock;

const MAX_TRANSFORMS: usize = 64;

fn main() -> Result<()> {
    let config_path = config_path()?;
    let source = read_config(&config_path)?;

    // Validate the supplied configuration before creating a local provider binding.
    let config = StaticExtrinsicsConfig::from_toml(&source, MAX_TRANSFORMS)
        .context("invalid static extrinsics config")?;

    // HostClock supplies an identified timestamp. Its read precision is unknown here.
    let host_clock = HostClock::new().context("could not identify the host monotonic clock")?;
    let effective_from = host_clock
        .monotonic_now()
        .context("could not read the host monotonic clock")?;
    let static_set = config.into_static_set(1, effective_from, None);

    // Unknown time precision is an explicit opt-in and remains visible in results.
    let buffer = TransformBuffer::new(
        TfResourceConfig::default(),
        LookupPolicy {
            allow_unknown_uncertainty: true,
            ..LookupPolicy::default()
        },
    )?;
    let authority = buffer.bind_provider("configured_extrinsics", CapabilityInstanceId::new())?;
    buffer.apply_static_snapshot(&authority, static_set)?;

    let source_point = Vec3::new(0.2, -0.1, 1.0)?;
    let forward = buffer.lookup_at("base_link", "camera_optical", effective_from)?;
    let base_point = forward.apply_point(source_point)?;
    let inverse = buffer.lookup_at("camera_optical", "base_link", effective_from)?;
    let recovered_source_point = inverse.apply_point(base_point)?;

    println!("Configured extrinsic — calibration accuracy unverified");
    println!("Software-only config: {}", config_path.display());
    println!(
        "timestamp: {} ns on {:?} generation {}",
        effective_from.nanoseconds(),
        effective_from.clock().kind(),
        effective_from.clock().generation()
    );
    println!("timing uncertainty: {:?}", forward.timing_uncertainty_ns);
    println!("source point in camera_optical: {:?}", source_point.xyz());
    println!("mapped point in base_link:      {:?}", base_point.xyz());
    println!(
        "inverse-mapped point:           {:?}",
        recovered_source_point.xyz()
    );
    print_sources("forward", &forward.sources);
    print_sources("inverse", &inverse.sources);
    Ok(())
}

fn config_path() -> Result<PathBuf> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("--config")) {
        bail!("usage: rbt-tf-local --config <path-to-static-extrinsics.toml>");
    }
    let path = args
        .next()
        .map(PathBuf::from)
        .context("missing config path")?;
    if args.next().is_some() {
        bail!("expected exactly one config path after --config");
    }
    Ok(path)
}

fn read_config(path: &Path) -> Result<String> {
    let file = std::fs::File::open(path)
        .with_context(|| format!("could not open config file {}", path.display()))?;
    let mut bytes = Vec::with_capacity(MAX_STATIC_CONFIG_BYTES.min(4096));
    file.take((MAX_STATIC_CONFIG_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .context("could not read config file")?;
    if bytes.len() > MAX_STATIC_CONFIG_BYTES {
        bail!("config file exceeds the 1 MiB input limit");
    }
    String::from_utf8(bytes).context("config file is not valid UTF-8")
}

fn print_sources(label: &str, sources: &[rbt_tf::TransformSource]) {
    for source in sources {
        println!(
            "{label} edge {}<-{}: revision {:?}, time {} ns, bracket {}..{} ns",
            source.parent,
            source.child,
            source.static_revision,
            source.timestamp.nanoseconds(),
            source.bracket_start.nanoseconds(),
            source.bracket_end.nanoseconds()
        );
    }
}

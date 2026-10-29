use std::path::PathBuf;

use anyhow::{Context, bail};
use rbt_compose::run_component;
use rbt_tf_camera::{
    CalibrationComponent, CalibrationConfig, CameraComponent, CameraConfig, PerceptionComponent,
    PerceptionConfig,
};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let mut args = std::env::args().skip(1);
    let role = args
        .next()
        .context("expected camera, calibration, or perception role")?;
    let mut values = std::collections::BTreeMap::<String, String>::new();
    while let Some(key) = args.next() {
        if !key.starts_with("--") {
            bail!("unexpected positional argument {key:?}");
        }
        let value = args
            .next()
            .with_context(|| format!("{key} requires a value"))?;
        if values.insert(key.clone(), value).is_some() {
            bail!("duplicate option {key}");
        }
    }
    let composer =
        std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned());
    match role.as_str() {
        "camera" => {
            let config = CameraConfig {
                component_id: take(&mut values, "--id")?
                    .unwrap_or_else(|| "physical-camera".into()),
                device: PathBuf::from(required(&mut values, "--device")?),
                camera_executable: PathBuf::from(required(&mut values, "--camera-exe")?),
                frame_id: take(&mut values, "--frame-id")?
                    .unwrap_or_else(|| "camera_optical".into()),
                duration_seconds: parse_duration(take(&mut values, "--duration-sec")?)?,
            };
            ensure_empty(&values)?;
            run_component::<CameraComponent, _>(config, composer).await?;
        }
        "calibration" => {
            let config = CalibrationConfig {
                component_id: take(&mut values, "--id")?
                    .unwrap_or_else(|| "camera-calibration".into()),
                path: PathBuf::from(required(&mut values, "--config")?),
            };
            ensure_empty(&values)?;
            run_component::<CalibrationComponent, _>(config, composer).await?;
        }
        "perception" => {
            let config = PerceptionConfig {
                component_id: take(&mut values, "--id")?.unwrap_or_else(|| "tf-perception".into()),
                frame_id: take(&mut values, "--frame-id")?
                    .unwrap_or_else(|| "camera_optical".into()),
                duration_seconds: parse_duration(take(&mut values, "--duration-sec")?)?,
            };
            ensure_empty(&values)?;
            run_component::<PerceptionComponent, _>(config, composer).await?;
        }
        _ => bail!("role must be camera, calibration, or perception"),
    }
    Ok(())
}

fn ensure_empty(values: &std::collections::BTreeMap<String, String>) -> anyhow::Result<()> {
    if !values.is_empty() {
        bail!(
            "unknown option(s): {}",
            values.keys().cloned().collect::<Vec<_>>().join(", ")
        );
    }
    Ok(())
}

fn take(
    values: &mut std::collections::BTreeMap<String, String>,
    key: &str,
) -> anyhow::Result<Option<String>> {
    Ok(values.remove(key))
}

fn required(
    values: &mut std::collections::BTreeMap<String, String>,
    key: &str,
) -> anyhow::Result<String> {
    values
        .remove(key)
        .with_context(|| format!("{key} is required"))
}

fn parse_duration(value: Option<String>) -> anyhow::Result<u64> {
    let value = value.context("--duration-sec is required")?;
    let duration = value
        .parse::<u64>()
        .context("--duration-sec must be an integer")?;
    if duration == 0 {
        bail!("--duration-sec must be positive");
    }
    Ok(duration)
}

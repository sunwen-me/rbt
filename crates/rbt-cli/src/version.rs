//! Product and frozen compatibility-axis reporting.

use std::fmt::Write as _;

use serde::{Deserialize, Serialize};

/// Versions that identify the product and each independent wire/ABI axis.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct VersionInfo {
    pub rbt_version: String,
    pub sdk_version: String,
    pub idl_language_version: u16,
    pub message_descriptor_version: u16,
    pub control_protocol_version: u16,
    pub composer_snapshot_version: u16,
    pub mcap_file_encoding_version: u16,
    pub iceoryx2_version: String,
}

impl VersionInfo {
    pub fn current() -> Self {
        Self {
            rbt_version: env!("CARGO_PKG_VERSION").to_owned(),
            sdk_version: env!("CARGO_PKG_VERSION").to_owned(),
            idl_language_version: rbt_idl::IDL_LANGUAGE_VERSION,
            message_descriptor_version: rbt_idl::MESSAGE_DESCRIPTOR_VERSION,
            control_protocol_version: rbt_compose::CONTROL_PROTOCOL_VERSION,
            composer_snapshot_version: rbt_compose::COMPOSER_SNAPSHOT_VERSION,
            mcap_file_encoding_version: rbt_types::CANONICAL_FILE_ENCODING_VERSION,
            iceoryx2_version: "0.10.0-rbt.2".to_owned(),
        }
    }
}

/// Render `rbt --version` or `rbt version --verbose`.
pub fn format_version(verbose: bool) -> String {
    let info = VersionInfo::current();
    let mut output = String::new();
    let _ = writeln!(output, "rbt {}", info.rbt_version);
    let _ = writeln!(output, "SDK {}", info.sdk_version);
    if verbose {
        let _ = writeln!(output, "IDL language {}", info.idl_language_version);
        let _ = writeln!(
            output,
            "message descriptor {}",
            info.message_descriptor_version
        );
        let _ = writeln!(output, "control protocol {}", info.control_protocol_version);
        let _ = writeln!(
            output,
            "Composer snapshot {}",
            info.composer_snapshot_version
        );
        let _ = writeln!(
            output,
            "MCAP file encoding {}",
            info.mcap_file_encoding_version
        );
        let _ = writeln!(output, "iceoryx2 {}", info.iceoryx2_version);
    }
    output
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn verbose_version_keeps_frozen_axes_explicit() {
        let info = VersionInfo::current();
        assert_eq!(info.rbt_version, "0.8.0");
        assert_eq!(info.sdk_version, "0.8.0");
        assert_eq!(info.idl_language_version, 1);
        assert_eq!(info.message_descriptor_version, 1);
        assert_eq!(info.control_protocol_version, 2);
        assert_eq!(info.composer_snapshot_version, 4);
        assert_eq!(info.mcap_file_encoding_version, 1);
        assert_eq!(info.iceoryx2_version, "0.10.0-rbt.2");
        let output = format_version(true);
        assert!(output.contains("control protocol 2"));
        assert!(output.contains("Composer snapshot 4"));
    }
}

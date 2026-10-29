use anyhow::{Context, Result, anyhow};
use tracing::warn;

/// Canonical environment variables used only by this example's deterministic
/// failure demonstrations. They intentionally do not belong to the public SDK.
pub(crate) const FAIL_AFTER_ENV: &str = "RBT_TEST_FAIL_AFTER";
pub(crate) const EXIT_AFTER_ENV: &str = "RBT_TEST_EXIT_AFTER";
pub(crate) const PAUSE_MS_ENV: &str = "RBT_TEST_PAUSE_MS";

/// Compatibility name retained for the phase-two demonstration. New tests
/// should use [`FAIL_AFTER_ENV`].
const DEPRECATED_CRASH_AFTER_ENV: &str = "RBT_TEST_CRASH_AFTER";

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub(crate) struct TestFaults {
    pub(crate) fail_after: Option<u64>,
    pub(crate) exit_after: Option<u64>,
    pub(crate) pause_ms: Option<u64>,
    pub(crate) used_deprecated_crash_after: bool,
}

impl TestFaults {
    pub(crate) fn from_env() -> Result<Self> {
        let fail_after = read_optional(FAIL_AFTER_ENV)?;
        let deprecated_fail_after = read_optional(DEPRECATED_CRASH_AFTER_ENV)?;
        let (fail_after, used_deprecated_crash_after) = match (fail_after, deprecated_fail_after) {
            (Some(value), Some(_)) => {
                warn!(
                    variable = DEPRECATED_CRASH_AFTER_ENV,
                    replacement = FAIL_AFTER_ENV,
                    "deprecated test fault variable ignored because its replacement is set"
                );
                (Some(value), false)
            }
            (Some(value), None) => (Some(value), false),
            (None, Some(value)) => {
                warn!(
                    variable = DEPRECATED_CRASH_AFTER_ENV,
                    replacement = FAIL_AFTER_ENV,
                    "deprecated test fault variable in use; migrate to the replacement"
                );
                (Some(value), true)
            }
            (None, None) => (None, false),
        };

        Ok(Self {
            fail_after,
            exit_after: read_optional(EXIT_AFTER_ENV)?,
            pause_ms: read_optional(PAUSE_MS_ENV)?,
            used_deprecated_crash_after,
        })
    }

    pub(crate) fn failure_word(self) -> &'static str {
        if self.used_deprecated_crash_after {
            "crash"
        } else {
            "failure"
        }
    }
}

fn read_optional(name: &'static str) -> Result<Option<u64>> {
    match std::env::var(name) {
        Ok(value) => value
            .parse::<u64>()
            .with_context(|| format!("parse {name}={value:?}"))
            .map(Some),
        Err(std::env::VarError::NotPresent) => Ok(None),
        Err(std::env::VarError::NotUnicode(_)) => {
            Err(anyhow!("parse {name}: value is not valid UTF-8"))
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn canonical_names_are_stable() {
        assert_eq!(super::FAIL_AFTER_ENV, "RBT_TEST_FAIL_AFTER");
        assert_eq!(super::EXIT_AFTER_ENV, "RBT_TEST_EXIT_AFTER");
        assert_eq!(super::PAUSE_MS_ENV, "RBT_TEST_PAUSE_MS");
    }

    #[test]
    fn failure_word_preserves_legacy_terminology() {
        assert_eq!(
            super::TestFaults {
                used_deprecated_crash_after: true,
                ..Default::default()
            }
            .failure_word(),
            "crash"
        );
        assert_eq!(super::TestFaults::default().failure_word(), "failure");
    }
}

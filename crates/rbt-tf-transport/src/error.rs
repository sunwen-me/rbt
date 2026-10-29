use thiserror::Error;

/// A failure at the TF transport or activation boundary.
#[derive(Debug, Error)]
pub enum TransportError {
    /// A TF validation, buffer, or graph operation failed.
    #[error(transparent)]
    Tf(#[from] rbt_tf::TfError),
    /// A native request/response operation failed.
    #[error(transparent)]
    Rpc(#[from] rbt_iox2::RpcError),
    /// A native stream operation failed.
    #[error(transparent)]
    Stream(#[from] rbt_iox2::StreamError),
    /// A native wait operation failed.
    #[error(transparent)]
    Wait(#[from] rbt_iox2::WaitError),
    /// A composition requirement could not be resolved.
    #[error(transparent)]
    Requirement(#[from] rbt_compose::RequirementError),
    /// The paired static update stream and snapshot RPC do not share a provider activation.
    #[error("static TF capability pairing is invalid: {0}")]
    PairingInvalid(&'static str),
    /// A descriptor's service name does not match its capability identity.
    #[error("TF service name does not match its capability identity")]
    ServiceIdentityMismatch,
    /// A static replacement does not advance one revision on a compatible clock.
    #[error("static TF replacement must advance one revision and effective time monotonically")]
    InvalidStaticReplacement,
    /// A requested stream drain exceeds the fixed fairness limit.
    #[error("TF drain budget {requested} exceeds the maximum {maximum}")]
    DrainBudget { requested: usize, maximum: usize },
    /// A provider is no longer active in its owning activation.
    #[error("TF provider activation has ended")]
    ActivationEnded,
    /// Shared endpoint state was poisoned by a panic while being mutated.
    #[error("TF transport state lock was poisoned: {0}")]
    LockPoisoned(&'static str),
    /// A snapshot RPC is already in flight for this client.
    #[error("a TF snapshot request is already in flight")]
    SnapshotInFlight,
}

/// Result type for TF transport operations.
pub type TransportResult<T> = Result<T, TransportError>;

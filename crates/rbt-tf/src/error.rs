//! Errors returned by frame and transform operations.

use thiserror::Error;

/// A frame or transform operation could not be completed.
#[derive(Clone, Debug, Error, PartialEq)]
pub enum TfError {
    /// A frame identifier does not follow the frame naming rules.
    #[error("frame identifier is invalid")]
    InvalidFrame,
    /// A requested frame is not known to the transform graph.
    #[error("frame is unknown")]
    UnknownFrame,
    /// No path connects the requested frames.
    #[error("frames are disconnected")]
    DisconnectedFrames,
    /// A transform required by the operation is missing.
    #[error("transform is missing")]
    MissingTransform,
    /// A frame already has a different parent.
    #[error("frame has a conflicting parent")]
    ConflictingParent,
    /// A transform has a conflicting authority.
    #[error("transform has a conflicting authority")]
    ConflictingAuthority,
    /// Adding the transform would introduce a cycle.
    #[error("transform graph contains a cycle")]
    CycleDetected,
    /// A transform contains invalid numeric values.
    #[error("transform is invalid")]
    InvalidTransform,
    /// A quaternion is non-finite or is not approximately unit length.
    #[error("quaternion is invalid")]
    InvalidQuaternion,
    /// The requested time is earlier than the available history.
    #[error("requested time is before available history")]
    PastExtrapolation,
    /// The requested time is later than the available history.
    #[error("requested time is after available history")]
    FutureExtrapolation,
    /// There is not enough history to answer the operation.
    #[error("transform history is insufficient")]
    InsufficientHistory,
    /// A transform interpolation interval exceeds its configured limit.
    #[error("transform interpolation gap is too large")]
    InterpolationGapTooLarge,
    /// The transform and query use different time epochs.
    #[error("transform epoch does not match")]
    TransformEpochMismatch,
    /// A transform provider is unavailable.
    #[error("transform provider is unavailable")]
    ProviderUnavailable,
    /// A transform snapshot is stale.
    #[error("transform snapshot is stale")]
    SnapshotStale,
    /// A transform snapshot revision is missing or discontinuous.
    #[error("transform snapshot revision has a gap")]
    SnapshotRevisionGap,
    /// A transform snapshot exceeds the configured size bound.
    #[error("transform snapshot is too large")]
    SnapshotTooLarge,
    /// A bounded transform resource has reached capacity.
    #[error("transform capacity is exceeded")]
    CapacityExceeded,
    /// A transform configuration is invalid.
    #[error("transform configuration is invalid")]
    InvalidConfig,
    /// A known temporal uncertainty exceeds the explicit lookup policy.
    #[error("timestamp uncertainty exceeds lookup policy")]
    TimestampUncertaintyExceeded,
    /// A transform sample arrived before the previously accepted sample.
    #[error("transform sample is out of order")]
    OutOfOrderSample,
    /// A clock or timestamp operation failed.
    #[error(transparent)]
    Time(#[from] rbt_time::TimeError),
}

/// The result type used by this crate.
pub type TfResult<T> = Result<T, TfError>;

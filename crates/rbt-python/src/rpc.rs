//! Synchronous and asyncio Python bindings for typed local RPC.
//!
//! The Python-facing objects in this module are deliberately only owners of
//! native rbt-iox2 loans and guards.  Request and response bytes never cross
//! the PyO3 boundary as `bytes` or a `Vec`; Python obtains a buffer view over
//! the native shared-memory allocation instead.  The small enum dispatch is
//! necessary because rbt-iox2's typed endpoint is monomorphised over the
//! generated fixed and dynamic message types.

use std::{
    ffi::CStr,
    mem::size_of,
    ops::Deref,
    sync::{
        Arc, Mutex, MutexGuard, Weak,
        atomic::{AtomicU64, Ordering},
    },
    thread::{self, JoinHandle},
    time::Duration,
};

use pyo3::{
    Bound, Py, PyAny, PyResult, Python,
    exceptions::PyValueError,
    prelude::*,
    types::{PyCFunction, PyDict, PyModule, PyTuple},
};
use rbt_core::{ActivationId, CapabilityInstanceId, MessageTypeDescriptor};
use rbt_iox2::{
    ControlCommand, Detection, EventSource, FixedSliceRpcClient, FixedSliceRpcServer, Frame, Image,
    ImageHeader, PendingRpcCall, PixelFormat, RpcClient, RpcDescriptor, RpcError as NativeRpcError,
    RpcRequest, RpcRequestLoan, RpcResponseGuard, RpcResponseLoan, RpcResult, RpcServer,
    RpcServerWakeHandle, SliceFixedRpcClient, SliceFixedRpcServer, SliceRpcClient, SliceRpcServer,
    WaitContext, WaitProgress, WaitToken,
};

use super::{
    AbiMismatchError, AsyncRuntimeClosedError, InvalidResourceConfigError,
    MissingTransportFeatureError, RbtError, RpcActivationExpiredError, RpcCancelledError,
    RpcCapacityError, RpcDeadlineExceededError, RpcTransportError, RpcUnavailableError,
};

/// The native endpoint can only instantiate message types that are linked into
/// this extension.  These are the fixed generated types shipped by rbt-types.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FixedMessageKind {
    Frame,
    Detection,
    ControlCommand,
}

fn fixed_kind(descriptor: &MessageTypeDescriptor) -> PyResult<FixedMessageKind> {
    if !descriptor.shape.is_fixed() {
        return Err(PyValueError::new_err(format!(
            "RPC request/response type {} is not fixed-size",
            descriptor.qualified_name()
        )));
    }
    let kind = match (descriptor.namespace.as_str(), descriptor.name.as_str()) {
        ("rbt.demo", "Frame") => FixedMessageKind::Frame,
        ("rbt.demo", "Detection") => FixedMessageKind::Detection,
        ("rbt.control", "ControlCommand") => FixedMessageKind::ControlCommand,
        _ => {
            return Err(PyValueError::new_err(format!(
                "fixed RPC message type {} is not linked into rbt._rbt",
                descriptor.qualified_name()
            )));
        }
    };
    Ok(kind)
}

fn is_image(descriptor: &MessageTypeDescriptor) -> bool {
    descriptor.shape.is_slice()
        && descriptor.namespace == "rbt.sensor"
        && descriptor.name == "Image"
}

fn rpc_descriptor(descriptor_json: &str) -> PyResult<RpcDescriptor> {
    serde_json::from_str(descriptor_json)
        .map_err(|error| PyValueError::new_err(format!("invalid RpcDescriptor JSON: {error}")))
}

fn activation_instance(value: &str) -> PyResult<CapabilityInstanceId> {
    serde_json::from_str(&format!("{value:?}"))
        .map_err(|error| PyValueError::new_err(format!("invalid capability instance id: {error}")))
}

fn checked_rpc_timeout(seconds: f64) -> PyResult<Duration> {
    if !seconds.is_finite() || seconds < 0.0 {
        return Err(PyValueError::new_err(
            "RPC timeout must be a finite non-negative number",
        ));
    }
    Duration::try_from_secs_f64(seconds)
        .map_err(|error| PyValueError::new_err(format!("invalid RPC timeout: {error}")))
}

type ImageHeaderTuple = (u64, u64, u32, u32, u32, u32);

fn image_header_tuple(header: ImageHeader) -> ImageHeaderTuple {
    (
        header.sequence,
        header.timestamp_ns,
        header.width,
        header.height,
        header.stride,
        header.encoding.value(),
    )
}

fn make_image_header(
    sequence: u64,
    timestamp_ns: u64,
    width: u32,
    height: u32,
    stride: u32,
    encoding: u32,
) -> ImageHeader {
    ImageHeader {
        sequence,
        timestamp_ns,
        width,
        height,
        stride,
        encoding: PixelFormat::new(encoding),
    }
}

fn not_image_payload() -> pyo3::PyErr {
    PyValueError::new_err("RPC payload is not rbt.sensor/Image")
}

pub(crate) fn rpc_py_error(error: NativeRpcError) -> pyo3::PyErr {
    let message = error.to_string();
    match error {
        NativeRpcError::AbiMismatch { .. } => AbiMismatchError::new_err(message),
        NativeRpcError::MissingTransportFeature { .. } => {
            MissingTransportFeatureError::new_err(message)
        }
        NativeRpcError::InvalidResourceConfig { .. } | NativeRpcError::ResourceOverflow { .. } => {
            InvalidResourceConfigError::new_err(message)
        }
        NativeRpcError::ClientCapacityReached
        | NativeRpcError::ServerCapacityReached
        | NativeRpcError::ResponseCapacityReached
        | NativeRpcError::InvalidSliceLength { .. }
        | NativeRpcError::LoanPoolExhausted(_)
        | NativeRpcError::BorrowLimitReached(_) => RpcCapacityError::new_err(message),
        NativeRpcError::ServerUnavailable => RpcUnavailableError::new_err(message),
        NativeRpcError::DeadlineExceeded => RpcDeadlineExceededError::new_err(message),
        NativeRpcError::Cancelled => RpcCancelledError::new_err(message),
        NativeRpcError::ActivationExpired => RpcActivationExpiredError::new_err(message),
        NativeRpcError::TransportFailure { .. }
        | NativeRpcError::ServiceName(_)
        | NativeRpcError::Node(_)
        | NativeRpcError::EventOpenOrCreate(_)
        | NativeRpcError::EventOpen(_)
        | NativeRpcError::Listener(_)
        | NativeRpcError::Notifier(_)
        | NativeRpcError::RequestResponseOpenOrCreate(_)
        | NativeRpcError::RequestResponseOpen(_)
        | NativeRpcError::ClientCreate(_)
        | NativeRpcError::ServerCreate(_) => RpcTransportError::new_err(message),
    }
}

macro_rules! fixed_rpc_variants {
    ($(($variant:ident, $q_kind:ident, $r_kind:ident, $q:ty, $r:ty)),+ $(,)?) => {
        pub(crate) enum RpcClientVariant {
            $( $variant(RpcClient<$q, $r>), )+
            FrameImage(FixedSliceRpcClient<Frame, Image>),
            ImageFrame(SliceFixedRpcClient<Image, Frame>),
            ImageImage(SliceRpcClient<Image, Image>),
        }

        pub(crate) enum RpcRequestLoanVariant {
            $( $variant(RpcRequestLoan<$q, $r>), )+
            FrameImage(RpcRequestLoan<Frame, [u8], (), ImageHeader>),
            ImageFrame(RpcRequestLoan<[u8], Frame, ImageHeader, ()>),
            ImageImage(RpcRequestLoan<[u8], [u8], ImageHeader, ImageHeader>),
        }

        pub(crate) enum RpcResponseGuardVariant {
            $( $variant(RpcResponseGuard<$r>), )+
            FrameImage(RpcResponseGuard<[u8], ImageHeader>),
            ImageFrame(RpcResponseGuard<Frame>),
            ImageImage(RpcResponseGuard<[u8], ImageHeader>),
        }

        pub(crate) enum RpcPendingCallVariant {
            $( $variant(PendingRpcCall<$q, $r>), )+
            FrameImage(PendingRpcCall<Frame, [u8], (), ImageHeader>),
            ImageFrame(PendingRpcCall<[u8], Frame, ImageHeader, ()>),
            ImageImage(PendingRpcCall<[u8], [u8], ImageHeader, ImageHeader>),
        }

        pub(crate) enum RpcServerVariant {
            $( $variant(RpcServer<$q, $r>), )+
            FrameImage(FixedSliceRpcServer<Frame, Image>),
            ImageFrame(SliceFixedRpcServer<Image, Frame>),
            ImageImage(SliceRpcServer<Image, Image>),
        }

        pub(crate) enum RpcServerWakeVariant {
            $( $variant(RpcServerWakeHandle), )+
            FrameImage(RpcServerWakeHandle),
            ImageFrame(RpcServerWakeHandle),
            ImageImage(RpcServerWakeHandle),
        }

        pub(crate) enum RpcRequestVariant {
            $( $variant(RpcRequest<$q, $r>), )+
            FrameImage(RpcRequest<Frame, [u8], (), ImageHeader>),
            ImageFrame(RpcRequest<[u8], Frame, ImageHeader, ()>),
            ImageImage(RpcRequest<[u8], [u8], ImageHeader, ImageHeader>),
        }

        pub(crate) enum RpcResponseLoanVariant {
            $( $variant(RpcResponseLoan<$q, $r>), )+
            FrameImage(RpcResponseLoan<Frame, [u8], (), ImageHeader>),
            ImageFrame(RpcResponseLoan<[u8], Frame, ImageHeader, ()>),
            ImageImage(RpcResponseLoan<[u8], [u8], ImageHeader, ImageHeader>),
        }

        impl RpcClientVariant {
            fn open(
                descriptor: &RpcDescriptor,
                identity: Option<(CapabilityInstanceId, ActivationId)>,
            ) -> RpcResult<Self> {
                if matches!(
                    fixed_kind(descriptor.request_type()),
                    Ok(FixedMessageKind::Frame)
                )
                    && is_image(descriptor.response_type())
                {
                    let client = match identity {
                        Some((capability, activation)) =>
                            FixedSliceRpcClient::<Frame, Image>::open_for_activation(
                                descriptor, capability, activation,
                            )?,
                        None => FixedSliceRpcClient::<Frame, Image>::open(descriptor)?,
                    };
                    return Ok(Self::FrameImage(client));
                }
                if is_image(descriptor.request_type())
                    && matches!(
                        fixed_kind(descriptor.response_type()),
                        Ok(FixedMessageKind::Frame)
                    )
                {
                    let client = match identity {
                        Some((capability, activation)) =>
                            SliceFixedRpcClient::<Image, Frame>::open_for_activation(
                                descriptor, capability, activation,
                            )?,
                        None => SliceFixedRpcClient::<Image, Frame>::open(descriptor)?,
                    };
                    return Ok(Self::ImageFrame(client));
                }
                if is_image(descriptor.request_type()) && is_image(descriptor.response_type()) {
                    let client = match identity {
                        Some((capability, activation)) =>
                            SliceRpcClient::<Image, Image>::open_for_activation(
                                descriptor, capability, activation,
                            )?,
                        None => SliceRpcClient::<Image, Image>::open(descriptor)?,
                    };
                    return Ok(Self::ImageImage(client));
                }
                match (fixed_kind(descriptor.request_type()), fixed_kind(descriptor.response_type())) {
                    $(
                        (Ok(FixedMessageKind::$q_kind), Ok(FixedMessageKind::$r_kind)) => {
                            let client = match identity {
                                Some((capability, activation)) =>
                                    RpcClient::<$q, $r>::open_for_activation(descriptor, capability, activation)?,
                                None => RpcClient::<$q, $r>::open(descriptor)?,
                            };
                            Ok(Self::$variant(client))
                        }
                    )+
                    _ => Err(NativeRpcError::TransportFailure {
                        operation: "open RPC client",
                        detail: format!(
                            "unsupported RPC type pair {}/{}",
                            descriptor.request_type().qualified_name(),
                            descriptor.response_type().qualified_name(),
                        ),
                    }),
                }
            }

            fn request_is_slice(&self) -> bool {
                match self {
                    $(Self::$variant(_) => false,)+
                    Self::FrameImage(_) => false,
                    Self::ImageFrame(_) | Self::ImageImage(_) => true,
                }
            }

            fn loan_request(&self, element_count: Option<usize>) -> RpcResult<RpcRequestLoanVariant> {
                match self {
                    $(Self::$variant(client) => client.loan_request().map(RpcRequestLoanVariant::$variant),)+
                    Self::FrameImage(client) => client.loan_request().map(RpcRequestLoanVariant::FrameImage),
                    Self::ImageFrame(client) => client
                        .loan_request_slice(element_count.expect("validated slice request length"))
                        .map(RpcRequestLoanVariant::ImageFrame),
                    Self::ImageImage(client) => client
                        .loan_request_slice(element_count.expect("validated slice request length"))
                        .map(RpcRequestLoanVariant::ImageImage),
                }
            }

            fn pressure(&self) -> rbt_core::RpcPressure {
                match self {
                    $(Self::$variant(client) => client.pressure_snapshot(),)+
                    Self::FrameImage(client) => client.pressure_snapshot(),
                    Self::ImageFrame(client) => client.pressure_snapshot(),
                    Self::ImageImage(client) => client.pressure_snapshot(),
                }
            }

            fn expire(&self) -> RpcResult<()> {
                match self {
                    $(Self::$variant(client) => client.expire(),)+
                    Self::FrameImage(client) => client.expire(),
                    Self::ImageFrame(client) => client.expire(),
                    Self::ImageImage(client) => client.expire(),
                }
            }
        }

        impl RpcRequestLoanVariant {
            fn parts(&self) -> (usize, usize) {
                match self {
                    $(Self::$variant(loan) => {
                        let value: &$q = loan.deref();
                        (value as *const $q as usize, size_of::<$q>())
                    }),+,
                    Self::FrameImage(loan) => {
                        let value: &Frame = loan.deref();
                        (value as *const Frame as usize, size_of::<Frame>())
                    }
                    Self::ImageFrame(loan) => (loan.as_ptr() as usize, loan.len()),
                    Self::ImageImage(loan) => (loan.as_ptr() as usize, loan.len()),
                }
            }

            fn image_header(&self) -> Option<ImageHeader> {
                match self {
                    Self::ImageFrame(loan) => Some(*loan.user_header()),
                    Self::ImageImage(loan) => Some(*loan.user_header()),
                    _ => None,
                }
            }

            fn set_image_header(&mut self, header: ImageHeader) -> bool {
                match self {
                    Self::ImageFrame(loan) => {
                        *loan.user_header_mut() = header;
                        true
                    }
                    Self::ImageImage(loan) => {
                        *loan.user_header_mut() = header;
                        true
                    }
                    _ => false,
                }
            }

            fn start(self) -> RpcResult<RpcPendingCallVariant> {
                match self {
                    $(Self::$variant(loan) => loan.send().map(RpcPendingCallVariant::$variant),)+
                    Self::FrameImage(loan) => loan.send().map(RpcPendingCallVariant::FrameImage),
                    Self::ImageFrame(loan) => loan.send().map(RpcPendingCallVariant::ImageFrame),
                    Self::ImageImage(loan) => loan.send().map(RpcPendingCallVariant::ImageImage),
                }
            }

            fn send(self, timeout: Duration) -> RpcResult<RpcResponseGuardVariant> {
                self.start()?.wait(timeout)
            }
        }

        impl RpcPendingCallVariant {
            fn wait(self, timeout: Duration) -> RpcResult<RpcResponseGuardVariant> {
                match self {
                    $(Self::$variant(call) => call.wait(timeout).map(RpcResponseGuardVariant::$variant),)+
                    Self::FrameImage(call) => call.wait(timeout).map(RpcResponseGuardVariant::FrameImage),
                    Self::ImageFrame(call) => call.wait(timeout).map(RpcResponseGuardVariant::ImageFrame),
                    Self::ImageImage(call) => call.wait(timeout).map(RpcResponseGuardVariant::ImageImage),
                }
            }

            fn response_event_source(&self, token: WaitToken) -> EventSource<'_> {
                match self {
                    $(Self::$variant(call) => call.response_event_source(token),)+
                    Self::FrameImage(call) => call.response_event_source(token),
                    Self::ImageFrame(call) => call.response_event_source(token),
                    Self::ImageImage(call) => call.response_event_source(token),
                }
            }

            fn cancellation_event_source(&self, token: WaitToken) -> EventSource<'_> {
                match self {
                    $(Self::$variant(call) => call.cancellation_event_source(token),)+
                    Self::FrameImage(call) => call.cancellation_event_source(token),
                    Self::ImageFrame(call) => call.cancellation_event_source(token),
                    Self::ImageImage(call) => call.cancellation_event_source(token),
                }
            }

            fn try_wait(&mut self) -> RpcResult<Option<RpcResponseGuardVariant>> {
                match self {
                    $(Self::$variant(call) => call.try_wait().map(|response| response.map(RpcResponseGuardVariant::$variant)),)+
                    Self::FrameImage(call) => call.try_wait().map(|response| response.map(RpcResponseGuardVariant::FrameImage)),
                    Self::ImageFrame(call) => call.try_wait().map(|response| response.map(RpcResponseGuardVariant::ImageFrame)),
                    Self::ImageImage(call) => call.try_wait().map(|response| response.map(RpcResponseGuardVariant::ImageImage)),
                }
            }

            fn deadline(self) -> RpcResult<RpcResponseGuardVariant> {
                match self {
                    $(Self::$variant(call) => call.wait(Duration::ZERO).map(RpcResponseGuardVariant::$variant),)+
                    Self::FrameImage(call) => call.wait(Duration::ZERO).map(RpcResponseGuardVariant::FrameImage),
                    Self::ImageFrame(call) => call.wait(Duration::ZERO).map(RpcResponseGuardVariant::ImageFrame),
                    Self::ImageImage(call) => call.wait(Duration::ZERO).map(RpcResponseGuardVariant::ImageImage),
                }
            }

            fn try_cancel(&self) -> bool {
                match self {
                    $(Self::$variant(call) => call.cancel_handle().try_cancel(),)+
                    Self::FrameImage(call) => call.cancel_handle().try_cancel(),
                    Self::ImageFrame(call) => call.cancel_handle().try_cancel(),
                    Self::ImageImage(call) => call.cancel_handle().try_cancel(),
                }
            }
        }

        impl RpcResponseGuardVariant {
            fn parts(&self) -> (usize, usize) {
                match self {
                    $(Self::$variant(response) => {
                        let value: &$r = response.deref();
                        (value as *const $r as usize, size_of::<$r>())
                    }),+,
                    Self::FrameImage(response) | Self::ImageImage(response) => {
                        (response.as_ptr() as usize, response.len())
                    }
                    Self::ImageFrame(response) => {
                        let value: &Frame = response.deref();
                        (value as *const Frame as usize, size_of::<Frame>())
                    }
                }
            }

            fn image_header(&self) -> Option<ImageHeader> {
                match self {
                    Self::FrameImage(response) | Self::ImageImage(response) => {
                        Some(*response.user_header())
                    }
                    _ => None,
                }
            }
        }

        impl RpcServerVariant {
            fn request_event_source(&self, token: WaitToken) -> EventSource<'_> {
                match self {
                    $(Self::$variant(server) => server.request_event_source(token),)+
                    Self::FrameImage(server) => server.request_event_source(token),
                    Self::ImageFrame(server) => server.request_event_source(token),
                    Self::ImageImage(server) => server.request_event_source(token),
                }
            }

            fn wake_handle(&self) -> RpcServerWakeVariant {
                match self {
                    $(Self::$variant(server) => {
                        RpcServerWakeVariant::$variant(server.wake_handle())
                    }),+,
                    Self::FrameImage(server) => RpcServerWakeVariant::FrameImage(server.wake_handle()),
                    Self::ImageFrame(server) => RpcServerWakeVariant::ImageFrame(server.wake_handle()),
                    Self::ImageImage(server) => RpcServerWakeVariant::ImageImage(server.wake_handle()),
                }
            }

            fn open(
                descriptor: &RpcDescriptor,
                capability: Option<CapabilityInstanceId>,
            ) -> RpcResult<Self> {
                if matches!(
                    fixed_kind(descriptor.request_type()),
                    Ok(FixedMessageKind::Frame)
                )
                    && is_image(descriptor.response_type())
                {
                    let server = match capability {
                        Some(capability) =>
                            FixedSliceRpcServer::<Frame, Image>::open_or_create_for_capability(
                                descriptor, capability,
                            )?,
                        None => FixedSliceRpcServer::<Frame, Image>::open_or_create(descriptor)?,
                    };
                    return Ok(Self::FrameImage(server));
                }
                if is_image(descriptor.request_type())
                    && matches!(
                        fixed_kind(descriptor.response_type()),
                        Ok(FixedMessageKind::Frame)
                    )
                {
                    let server = match capability {
                        Some(capability) =>
                            SliceFixedRpcServer::<Image, Frame>::open_or_create_for_capability(
                                descriptor, capability,
                            )?,
                        None => SliceFixedRpcServer::<Image, Frame>::open_or_create(descriptor)?,
                    };
                    return Ok(Self::ImageFrame(server));
                }
                if is_image(descriptor.request_type()) && is_image(descriptor.response_type()) {
                    let server = match capability {
                        Some(capability) =>
                            SliceRpcServer::<Image, Image>::open_or_create_for_capability(
                                descriptor, capability,
                            )?,
                        None => SliceRpcServer::<Image, Image>::open_or_create(descriptor)?,
                    };
                    return Ok(Self::ImageImage(server));
                }
                match (fixed_kind(descriptor.request_type()), fixed_kind(descriptor.response_type())) {
                    $(
                        (Ok(FixedMessageKind::$q_kind), Ok(FixedMessageKind::$r_kind)) => {
                            let server = match capability {
                                Some(capability) => RpcServer::<$q, $r>::open_or_create_for_capability(descriptor, capability)?,
                                None => RpcServer::<$q, $r>::open_or_create(descriptor)?,
                            };
                            Ok(Self::$variant(server))
                        }
                    )+
                    _ => Err(NativeRpcError::TransportFailure {
                        operation: "open RPC server",
                        detail: format!(
                            "unsupported RPC type pair {}/{}",
                            descriptor.request_type().qualified_name(),
                            descriptor.response_type().qualified_name(),
                        ),
                    }),
                }
            }

            fn try_receive(&mut self) -> RpcResult<Option<RpcRequestVariant>> {
                match self {
                    $(Self::$variant(server) => server.try_receive().map(|request| request.map(RpcRequestVariant::$variant)),)+
                    Self::FrameImage(server) => server.try_receive().map(|request| request.map(RpcRequestVariant::FrameImage)),
                    Self::ImageFrame(server) => server.try_receive().map(|request| request.map(RpcRequestVariant::ImageFrame)),
                    Self::ImageImage(server) => server.try_receive().map(|request| request.map(RpcRequestVariant::ImageImage)),
                }
            }

            fn receive_timeout(&mut self, timeout: Duration) -> RpcResult<Option<RpcRequestVariant>> {
                match self {
                    $(Self::$variant(server) => server.receive_timeout(timeout).map(|request| request.map(RpcRequestVariant::$variant)),)+
                    Self::FrameImage(server) => server.receive_timeout(timeout).map(|request| request.map(RpcRequestVariant::FrameImage)),
                    Self::ImageFrame(server) => server.receive_timeout(timeout).map(|request| request.map(RpcRequestVariant::ImageFrame)),
                    Self::ImageImage(server) => server.receive_timeout(timeout).map(|request| request.map(RpcRequestVariant::ImageImage)),
                }
            }

            fn receive(&mut self) -> RpcResult<RpcRequestVariant> {
                match self {
                    $(Self::$variant(server) => server.receive().map(RpcRequestVariant::$variant),)+
                    Self::FrameImage(server) => server.receive().map(RpcRequestVariant::FrameImage),
                    Self::ImageFrame(server) => server.receive().map(RpcRequestVariant::ImageFrame),
                    Self::ImageImage(server) => server.receive().map(RpcRequestVariant::ImageImage),
                }
            }

            fn pressure(&self) -> rbt_core::RpcPressure {
                match self {
                    $(Self::$variant(server) => server.pressure_snapshot(),)+
                    Self::FrameImage(server) => server.pressure_snapshot(),
                    Self::ImageFrame(server) => server.pressure_snapshot(),
                    Self::ImageImage(server) => server.pressure_snapshot(),
                }
            }

        }

        impl RpcServerWakeVariant {
            fn expire(&self) -> RpcResult<()> {
                match self {
                    $(Self::$variant(wake) => wake.expire(),)+
                    Self::FrameImage(wake) => wake.expire(),
                    Self::ImageFrame(wake) => wake.expire(),
                    Self::ImageImage(wake) => wake.expire(),
                }
            }
        }

        impl RpcRequestVariant {
            fn parts(&self) -> (usize, usize) {
                match self {
                    $(Self::$variant(request) => {
                        let value: &$q = request.deref();
                        (value as *const $q as usize, size_of::<$q>())
                    }),+,
                    Self::FrameImage(request) => {
                        let value: &Frame = request.deref();
                        (value as *const Frame as usize, size_of::<Frame>())
                    }
                    Self::ImageFrame(request) => (request.as_ptr() as usize, request.len()),
                    Self::ImageImage(request) => (request.as_ptr() as usize, request.len()),
                }
            }

            fn response_is_slice(&self) -> bool {
                match self {
                    $(Self::$variant(_) => false,)+
                    Self::FrameImage(_) | Self::ImageImage(_) => true,
                    Self::ImageFrame(_) => false,
                }
            }

            fn image_header(&self) -> Option<ImageHeader> {
                match self {
                    Self::ImageFrame(request) => Some(*request.user_header()),
                    Self::ImageImage(request) => Some(*request.user_header()),
                    _ => None,
                }
            }

            fn loan_response(self, element_count: Option<usize>) -> RpcResult<RpcResponseLoanVariant> {
                match self {
                    $(Self::$variant(request) => request.loan_response().map(RpcResponseLoanVariant::$variant),)+
                    Self::FrameImage(request) => request
                        .loan_response_slice(element_count.expect("validated slice response length"))
                        .map(RpcResponseLoanVariant::FrameImage),
                    Self::ImageFrame(request) => request
                        .loan_response()
                        .map(RpcResponseLoanVariant::ImageFrame),
                    Self::ImageImage(request) => request
                        .loan_response_slice(element_count.expect("validated slice response length"))
                        .map(RpcResponseLoanVariant::ImageImage),
                }
            }
        }

        impl RpcResponseLoanVariant {
            fn parts(&self) -> (usize, usize) {
                match self {
                    $(Self::$variant(response) => {
                        let value: &$r = response.deref();
                        (value as *const $r as usize, size_of::<$r>())
                    }),+,
                    Self::FrameImage(response) => (response.as_ptr() as usize, response.len()),
                    Self::ImageFrame(response) => {
                        let value: &Frame = response.deref();
                        (value as *const Frame as usize, size_of::<Frame>())
                    }
                    Self::ImageImage(response) => (response.as_ptr() as usize, response.len()),
                }
            }

            fn image_header(&self) -> Option<ImageHeader> {
                match self {
                    Self::FrameImage(response) => Some(*response.user_header()),
                    Self::ImageImage(response) => Some(*response.user_header()),
                    _ => None,
                }
            }

            fn set_image_header(&mut self, header: ImageHeader) -> bool {
                match self {
                    Self::FrameImage(response) => {
                        *response.user_header_mut() = header;
                        true
                    }
                    Self::ImageImage(response) => {
                        *response.user_header_mut() = header;
                        true
                    }
                    _ => false,
                }
            }

            fn send(self) -> RpcResult<()> {
                match self {
                    $(Self::$variant(response) => response.send(),)+
                    Self::FrameImage(response) => response.send(),
                    Self::ImageFrame(response) => response.send(),
                    Self::ImageImage(response) => response.send(),
                }
            }
        }
    };
}

fixed_rpc_variants! {
    (FrameFrame, Frame, Frame, Frame, Frame),
    (FrameDetection, Frame, Detection, Frame, Detection),
    (FrameControlCommand, Frame, ControlCommand, Frame, ControlCommand),
    (DetectionFrame, Detection, Frame, Detection, Frame),
    (DetectionDetection, Detection, Detection, Detection, Detection),
    (DetectionControlCommand, Detection, ControlCommand, Detection, ControlCommand),
    (ControlCommandFrame, ControlCommand, Frame, ControlCommand, Frame),
    (ControlCommandDetection, ControlCommand, Detection, ControlCommand, Detection),
    (ControlCommandControlCommand, ControlCommand, ControlCommand, ControlCommand, ControlCommand),
}

const ASYNC_OPEN: u8 = 0;
const ASYNC_COMPLETED: u8 = 1;
const ASYNC_CANCELLED: u8 = 2;
const ASYNC_EXPIRED: u8 = 3;
const ASYNC_DEADLINE: u8 = 4;
const ASYNC_FAILED: u8 = 5;

/// Lifecycle hook used by an activation guard to stop an RPC dispatcher before
/// its endpoints are torn down.
pub(crate) trait RpcAsyncBridgeControl: Send + Sync {
    fn expire_and_stop(&self);
}

enum RpcClientAsyncOutcome {
    Response(RpcResponseGuardVariant),
    Error(NativeRpcError),
}

struct RpcClientAsyncCallState {
    bridge: Weak<RpcClientAsyncBridge>,
    loop_object: Arc<Py<PyAny>>,
    future: Arc<Py<PyAny>>,
    pending: Mutex<Option<RpcPendingCallVariant>>,
    outcome: Mutex<Option<RpcClientAsyncOutcome>>,
    terminal: std::sync::atomic::AtomicU8,
}

impl RpcClientAsyncCallState {
    fn new(
        bridge: &Arc<RpcClientAsyncBridge>,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> Arc<Self> {
        Arc::new(Self {
            bridge: Arc::downgrade(bridge),
            loop_object: Arc::new(loop_object.unbind()),
            future: Arc::new(future.unbind()),
            pending: Mutex::new(None),
            outcome: Mutex::new(None),
            terminal: std::sync::atomic::AtomicU8::new(ASYNC_OPEN),
        })
    }

    fn is_open(&self) -> bool {
        self.terminal.load(Ordering::Acquire) == ASYNC_OPEN
    }

    fn set_pending(&self, pending: RpcPendingCallVariant) {
        let mut pending_slot = self
            .pending
            .lock()
            .expect("RPC async pending lock poisoned");
        if self.is_open() {
            *pending_slot = Some(pending);
            drop(pending_slot);
            if let Some(bridge) = self.bridge.upgrade() {
                bridge.wake_current();
            }
        } else {
            let _ = pending.try_cancel();
            drop(pending);
        }
    }

    fn complete(&self, terminal: u8, outcome: RpcClientAsyncOutcome) {
        if self
            .terminal
            .compare_exchange(ASYNC_OPEN, terminal, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        if let Ok(mut current) = self.outcome.lock() {
            *current = Some(outcome);
        }
        self.schedule_future();
    }

    fn schedule_future(&self) {
        let loop_object = Arc::clone(&self.loop_object);
        let future = Arc::clone(&self.future);
        Python::attach(|py| {
            let callback = PyCFunction::new_closure(
                py,
                None::<&CStr>,
                None::<&CStr>,
                move |args: &Bound<'_, PyTuple>,
                      _kwargs: Option<&Bound<'_, PyDict>>|
                      -> PyResult<Py<PyAny>> {
                    let py = args.py();
                    let future = future.bind(py);
                    let done = future
                        .call_method0("done")
                        .and_then(|done| done.extract::<bool>())
                        .unwrap_or(true);
                    if !done {
                        future.call_method1("set_result", (py.None(),))?;
                    }
                    Ok(py.None())
                },
            );
            if let Ok(callback) = callback {
                let _ = loop_object
                    .bind(py)
                    .call_method1("call_soon_threadsafe", (callback,));
            }
        });
    }

    fn poll(&self) {
        if !self.is_open() {
            return;
        }
        let mut pending_slot = self
            .pending
            .lock()
            .expect("RPC async pending lock poisoned");
        let Some(pending) = pending_slot.as_mut() else {
            return;
        };
        match pending.try_wait() {
            Ok(Some(response)) => {
                pending_slot.take();
                drop(pending_slot);
                self.complete(ASYNC_COMPLETED, RpcClientAsyncOutcome::Response(response));
            }
            Ok(None) => {}
            Err(error) => {
                pending_slot.take();
                drop(pending_slot);
                let terminal = match error {
                    NativeRpcError::ActivationExpired => ASYNC_EXPIRED,
                    NativeRpcError::Cancelled => ASYNC_CANCELLED,
                    _ => ASYNC_FAILED,
                };
                self.complete(terminal, RpcClientAsyncOutcome::Error(error));
            }
        }
    }

    fn deadline(&self) {
        if !self.is_open() {
            return;
        }
        let mut pending_slot = self
            .pending
            .lock()
            .expect("RPC async pending lock poisoned");
        let Some(pending) = pending_slot.take() else {
            return;
        };
        let result = pending.deadline();
        drop(pending_slot);
        match result {
            Ok(response) => {
                self.complete(ASYNC_COMPLETED, RpcClientAsyncOutcome::Response(response))
            }
            Err(error) => {
                let terminal = match error {
                    NativeRpcError::ActivationExpired => ASYNC_EXPIRED,
                    NativeRpcError::Cancelled => ASYNC_CANCELLED,
                    NativeRpcError::DeadlineExceeded => ASYNC_DEADLINE,
                    _ => ASYNC_FAILED,
                };
                self.complete(terminal, RpcClientAsyncOutcome::Error(error));
            }
        }
    }

    fn cancel(&self) {
        if !self.is_open() {
            return;
        }
        let mut pending_slot = self
            .pending
            .lock()
            .expect("RPC async pending lock poisoned");
        let claimed = pending_slot
            .as_ref()
            .map(|pending| pending.try_cancel())
            .unwrap_or(true);
        if claimed {
            let _ = self.terminal.compare_exchange(
                ASYNC_OPEN,
                ASYNC_CANCELLED,
                Ordering::AcqRel,
                Ordering::Acquire,
            );
            pending_slot.take();
        }
        drop(pending_slot);
        if let Some(bridge) = self.bridge.upgrade() {
            bridge.wake_current();
        }
    }

    fn expire(&self) {
        if !self.is_open() {
            return;
        }
        self.poll();
        if self.is_open() {
            let mut pending_slot = self
                .pending
                .lock()
                .expect("RPC async pending lock poisoned");
            pending_slot.take();
            drop(pending_slot);
            self.complete(
                ASYNC_EXPIRED,
                RpcClientAsyncOutcome::Error(NativeRpcError::ActivationExpired),
            );
        }
    }

    fn take_outcome(&self) -> PyResult<PyRpcResponse> {
        let outcome = self
            .outcome
            .lock()
            .map_err(|_| RbtError::new_err("RPC async outcome lock was poisoned"))?
            .take();
        match outcome {
            Some(RpcClientAsyncOutcome::Response(response)) => Ok(PyRpcResponse {
                shared: response_guard_shared(response),
            }),
            Some(RpcClientAsyncOutcome::Error(error)) => Err(rpc_py_error(error)),
            None => Err(RbtError::new_err("RPC async call has no terminal outcome")),
        }
    }

    fn terminal(&self) -> u8 {
        self.terminal.load(Ordering::Acquire)
    }
}

struct RpcClientBridgeState {
    calls: Vec<Arc<RpcClientAsyncCallState>>,
    wake: Option<rbt_iox2::WaitWakeHandle>,
    started: bool,
    stopped: bool,
    thread: Option<JoinHandle<()>>,
}

pub(crate) struct RpcClientAsyncBridge {
    process: rbt_core::ProcessInstanceId,
    activation: ActivationId,
    client: RpcClientCell,
    next_wait_activation: AtomicU64,
    state: Mutex<RpcClientBridgeState>,
}

impl RpcClientAsyncBridge {
    fn new(
        client: RpcClientCell,
        _process: rbt_core::ProcessInstanceId,
        activation: ActivationId,
    ) -> Arc<Self> {
        Arc::new(Self {
            // Each endpoint bridge gets an isolated cancellation namespace.
            process: rbt_core::ProcessInstanceId::new(),
            activation,
            client,
            next_wait_activation: AtomicU64::new(1),
            state: Mutex::new(RpcClientBridgeState {
                calls: Vec::new(),
                wake: None,
                started: false,
                stopped: false,
                thread: None,
            }),
        })
    }

    fn start(self: &Arc<Self>) {
        let spawn = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            if state.started || state.stopped {
                return;
            }
            state.started = true;
            true
        };
        if !spawn {
            return;
        }
        let bridge = Arc::clone(self);
        let thread = thread::Builder::new()
            .name(format!("rbt-rpc-client-bridge-{}", self.activation.value()))
            .spawn(move || run_rpc_client_bridge(bridge));
        match thread {
            Ok(thread) => {
                if let Ok(mut state) = self.state.lock() {
                    state.thread = Some(thread);
                }
            }
            Err(error) => {
                if let Ok(mut state) = self.state.lock() {
                    state.stopped = true;
                    let calls = std::mem::take(&mut state.calls);
                    drop(state);
                    for call in calls {
                        call.complete(
                            ASYNC_FAILED,
                            RpcClientAsyncOutcome::Error(NativeRpcError::TransportFailure {
                                operation: "start RPC asyncio bridge",
                                detail: error.to_string(),
                            }),
                        );
                    }
                }
            }
        }
    }

    fn register(self: &Arc<Self>, call: Arc<RpcClientAsyncCallState>) -> PyResult<()> {
        self.start();
        let mut state = self
            .state
            .lock()
            .map_err(|_| RbtError::new_err("RPC asyncio bridge state lock was poisoned"))?;
        if state.stopped {
            return Err(AsyncRuntimeClosedError::new_err(
                "RPC asyncio bridge is closed",
            ));
        }
        state.calls.push(call);
        let wake = state.wake.clone();
        drop(state);
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
        Ok(())
    }

    fn wake_current(&self) {
        let wake = self.state.lock().ok().and_then(|state| state.wake.clone());
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
    }

    fn pending_count(&self) -> usize {
        self.state
            .lock()
            .map(|state| state.calls.iter().filter(|call| call.is_open()).count())
            .unwrap_or(0)
    }

    fn stop_and_join(&self) {
        let (wake, thread, calls) = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            state.stopped = true;
            (
                state.wake.clone(),
                if state
                    .thread
                    .as_ref()
                    .is_some_and(|thread| thread.thread().id() == thread::current().id())
                {
                    None
                } else {
                    state.thread.take()
                },
                state.calls.clone(),
            )
        };
        if let Ok(endpoint) = self.client.lock()
            && let Some(endpoint) = endpoint.as_ref()
        {
            let _ = endpoint.expire();
        }
        for call in calls {
            call.expire();
        }
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
        if let Some(thread) = thread {
            let _ = thread.join();
        }
    }
}

impl RpcAsyncBridgeControl for RpcClientAsyncBridge {
    fn expire_and_stop(&self) {
        self.stop_and_join();
    }
}

impl Drop for RpcClientAsyncBridge {
    fn drop(&mut self) {
        self.stop_and_join();
    }
}

fn run_rpc_client_bridge(bridge: Arc<RpcClientAsyncBridge>) {
    loop {
        let calls = {
            let Ok(mut state) = bridge.state.lock() else {
                return;
            };
            if state.stopped {
                return;
            }
            state.calls.retain(|call| call.is_open());
            state.calls.clone()
        };
        // Probe the native correlation channel before attaching the sidecar
        // sources. This closes the send/attach race without relying on an
        // advisory notification.
        for call in &calls {
            call.poll();
        }
        let wait_activation = ActivationId::new(
            bridge
                .activation
                .value()
                .saturating_add(bridge.next_wait_activation.fetch_add(1, Ordering::Relaxed)),
        );
        let mut context = match WaitContext::new(bridge.process, wait_activation) {
            Ok(context) => context,
            Err(error) => {
                for call in calls {
                    call.complete(
                        ASYNC_FAILED,
                        RpcClientAsyncOutcome::Error(NativeRpcError::TransportFailure {
                            operation: "create RPC asyncio WaitSet",
                            detail: error.to_string(),
                        }),
                    );
                }
                return;
            }
        };
        let wake = context.wake_handle();
        if let Ok(mut state) = bridge.state.lock() {
            state.wake = Some(wake);
            if state.stopped {
                continue;
            }
        }

        let mut pending_guards = Vec::with_capacity(calls.len());
        for call in &calls {
            pending_guards.push(
                call.pending
                    .lock()
                    .expect("RPC async pending lock poisoned"),
            );
        }
        let mut sources = Vec::with_capacity(calls.len().saturating_mul(2));
        for (index, pending) in pending_guards.iter().enumerate() {
            let Some(pending) = pending.as_ref() else {
                continue;
            };
            let response_token = WaitToken::new((index as u64).saturating_mul(2).saturating_add(1));
            let cancellation_token =
                WaitToken::new((index as u64).saturating_mul(2).saturating_add(2));
            sources.push(pending.response_event_source(response_token));
            sources.push(pending.cancellation_event_source(cancellation_token));
        }
        let wait_result =
            context.run_with_timeout(&sources, Duration::from_millis(100), |event, _budget| {
                if !event.is_synthetic() {
                    Ok::<WaitProgress, rbt_iox2::WaitError>(WaitProgress::Stop)
                } else {
                    Ok::<WaitProgress, rbt_iox2::WaitError>(WaitProgress::Continue)
                }
            });
        drop(sources);
        drop(pending_guards);
        if let Err(error) = wait_result {
            for call in calls {
                call.complete(
                    ASYNC_FAILED,
                    RpcClientAsyncOutcome::Error(NativeRpcError::TransportFailure {
                        operation: "run RPC asyncio WaitSet",
                        detail: error.to_string(),
                    }),
                );
            }
            return;
        }
        // Sidecars are advisory. Probe every pending native call after both
        // a sidecar wake and the bounded native WaitSet safety deadline so a
        // coalesced/lost notification cannot strand its asyncio Future.
        for call in calls {
            call.poll();
        }
    }
}

enum RpcServerAsyncOutcome {
    Request(RpcRequestVariant),
    Error(NativeRpcError),
}

struct RpcServerAsyncReceiveState {
    bridge: Weak<RpcServerAsyncBridge>,
    loop_object: Arc<Py<PyAny>>,
    future: Arc<Py<PyAny>>,
    ownership: Mutex<Option<RpcReceiveOwnership>>,
    outcome: Mutex<Option<RpcServerAsyncOutcome>>,
    terminal: std::sync::atomic::AtomicU8,
}

impl RpcServerAsyncReceiveState {
    fn new(
        bridge: &Arc<RpcServerAsyncBridge>,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
        ownership: RpcReceiveOwnership,
    ) -> Arc<Self> {
        Arc::new(Self {
            bridge: Arc::downgrade(bridge),
            loop_object: Arc::new(loop_object.unbind()),
            future: Arc::new(future.unbind()),
            ownership: Mutex::new(Some(ownership)),
            outcome: Mutex::new(None),
            terminal: std::sync::atomic::AtomicU8::new(ASYNC_OPEN),
        })
    }

    fn release_ownership(&self) {
        let ownership = match self.ownership.lock() {
            Ok(mut ownership) => ownership.take(),
            Err(poisoned) => poisoned.into_inner().take(),
        };
        drop(ownership);
    }

    fn is_open(&self) -> bool {
        self.terminal.load(Ordering::Acquire) == ASYNC_OPEN
    }

    fn complete(&self, terminal: u8, outcome: RpcServerAsyncOutcome) {
        if self
            .terminal
            .compare_exchange(ASYNC_OPEN, terminal, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        if let Ok(mut current) = self.outcome.lock() {
            *current = Some(outcome);
        }
        if let Some(bridge) = self.bridge.upgrade() {
            bridge.remove(self);
        } else {
            self.release_ownership();
        }
        let loop_object = Arc::clone(&self.loop_object);
        let future = Arc::clone(&self.future);
        Python::attach(|py| {
            let callback = PyCFunction::new_closure(
                py,
                None::<&CStr>,
                None::<&CStr>,
                move |args: &Bound<'_, PyTuple>,
                      _kwargs: Option<&Bound<'_, PyDict>>|
                      -> PyResult<Py<PyAny>> {
                    let py = args.py();
                    let future = future.bind(py);
                    if !future
                        .call_method0("done")
                        .and_then(|done| done.extract::<bool>())
                        .unwrap_or(true)
                    {
                        future.call_method1("set_result", (py.None(),))?;
                    }
                    Ok(py.None())
                },
            );
            if let Ok(callback) = callback {
                let _ = loop_object
                    .bind(py)
                    .call_method1("call_soon_threadsafe", (callback,));
            }
        });
    }

    fn cancel(&self) -> bool {
        if self
            .terminal
            .compare_exchange(
                ASYNC_OPEN,
                ASYNC_CANCELLED,
                Ordering::AcqRel,
                Ordering::Acquire,
            )
            .is_ok()
        {
            if let Some(bridge) = self.bridge.upgrade() {
                bridge.remove(self);
            } else {
                self.release_ownership();
            }
            true
        } else {
            false
        }
    }

    fn expire(&self) {
        self.complete(
            ASYNC_EXPIRED,
            RpcServerAsyncOutcome::Error(NativeRpcError::ActivationExpired),
        );
    }

    fn take_outcome(&self) -> PyResult<PyRpcRequest> {
        let outcome = self
            .outcome
            .lock()
            .map_err(|_| RbtError::new_err("RPC async outcome lock was poisoned"))?
            .take();
        match outcome {
            Some(RpcServerAsyncOutcome::Request(request)) => Ok(PyRpcRequest {
                shared: request_shared(request),
            }),
            Some(RpcServerAsyncOutcome::Error(error)) => Err(rpc_py_error(error)),
            None => Err(RbtError::new_err(
                "RPC async receive has no terminal outcome",
            )),
        }
    }
}

struct RpcServerBridgeState {
    receive: Option<Arc<RpcServerAsyncReceiveState>>,
    wake: Option<rbt_iox2::WaitWakeHandle>,
    started: bool,
    stopped: bool,
    thread: Option<JoinHandle<()>>,
}

pub(crate) struct RpcServerAsyncBridge {
    process: rbt_core::ProcessInstanceId,
    activation: ActivationId,
    server: RpcServerCell,
    next_wait_activation: AtomicU64,
    state: Mutex<RpcServerBridgeState>,
}

impl RpcServerAsyncBridge {
    fn new(
        server: RpcServerCell,
        _process: rbt_core::ProcessInstanceId,
        activation: ActivationId,
    ) -> Arc<Self> {
        Arc::new(Self {
            process: rbt_core::ProcessInstanceId::new(),
            activation,
            server,
            next_wait_activation: AtomicU64::new(1),
            state: Mutex::new(RpcServerBridgeState {
                receive: None,
                wake: None,
                started: false,
                stopped: false,
                thread: None,
            }),
        })
    }

    fn start(self: &Arc<Self>) -> Result<(), String> {
        let Ok(mut state) = self.state.lock() else {
            return Err("RPC asyncio bridge state lock was poisoned".to_owned());
        };
        if state.started || state.stopped {
            return if state.stopped {
                Err("RPC asyncio bridge is closed".to_owned())
            } else {
                Ok(())
            };
        }
        state.started = true;
        drop(state);
        let bridge = Arc::clone(self);
        let thread = thread::Builder::new()
            .name(format!("rbt-rpc-server-bridge-{}", self.activation.value()))
            .spawn(move || run_rpc_server_bridge(bridge));
        match thread {
            Ok(thread) => {
                if let Ok(mut state) = self.state.lock() {
                    state.thread = Some(thread);
                }
                Ok(())
            }
            Err(error) => {
                if let Ok(mut state) = self.state.lock() {
                    state.stopped = true;
                }
                Err(error.to_string())
            }
        }
    }

    fn register(
        self: &Arc<Self>,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<Arc<RpcServerAsyncReceiveState>> {
        self.start().map_err(AsyncRuntimeClosedError::new_err)?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| RbtError::new_err("RPC asyncio bridge state lock was poisoned"))?;
        if state.stopped {
            return Err(AsyncRuntimeClosedError::new_err(
                "RPC asyncio bridge is closed",
            ));
        }
        if state
            .receive
            .as_ref()
            .is_some_and(|current| current.is_open())
        {
            return Err(crate::ConcurrentReceiveError::new_err(
                "only one receive_async waiter may be pending per RPC server",
            ));
        }
        let ownership = self.server.try_claim_receive().ok_or_else(|| {
            crate::ConcurrentReceiveError::new_err(
                "receive conflicts with another synchronous or asynchronous receive",
            )
        })?;
        let receive = RpcServerAsyncReceiveState::new(self, loop_object, future, ownership);
        state.receive = Some(Arc::clone(&receive));
        let wake = state.wake.clone();
        drop(state);
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
        Ok(receive)
    }

    fn remove(&self, receive: &RpcServerAsyncReceiveState) {
        let stopping = if let Ok(mut state) = self.state.lock() {
            let matches = state
                .receive
                .as_ref()
                .is_some_and(|current| std::ptr::eq(Arc::as_ptr(current), receive));
            if matches {
                state.receive = None;
            }
            matches && state.stopped
        } else {
            true
        };
        if !stopping {
            receive.release_ownership();
        }
        self.wake_current();
    }

    fn wake_current(&self) {
        let wake = self.state.lock().ok().and_then(|state| state.wake.clone());
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
    }

    fn pending_count(&self) -> usize {
        self.state
            .lock()
            .ok()
            .and_then(|state| {
                state
                    .receive
                    .as_ref()
                    .map(|receive| usize::from(receive.is_open()))
            })
            .unwrap_or(0)
    }

    fn stop_and_join(&self) {
        let (wake, thread, receive) = {
            let Ok(mut state) = self.state.lock() else {
                return;
            };
            state.stopped = true;
            (
                state.wake.clone(),
                if state
                    .thread
                    .as_ref()
                    .is_some_and(|thread| thread.thread().id() == thread::current().id())
                {
                    None
                } else {
                    state.thread.take()
                },
                state.receive.clone(),
            )
        };
        let _ = self.server.wake.expire();
        if let Some(ref receive) = receive {
            receive.expire();
        }
        if let Some(wake) = wake {
            let _ = wake.wake();
        }
        if let Some(thread) = thread {
            let _ = thread.join();
        }
        if let Some(receive) = receive {
            receive.release_ownership();
        }
    }
}

impl RpcAsyncBridgeControl for RpcServerAsyncBridge {
    fn expire_and_stop(&self) {
        self.stop_and_join();
    }
}

impl Drop for RpcServerAsyncBridge {
    fn drop(&mut self) {
        self.stop_and_join();
    }
}

fn run_rpc_server_bridge(bridge: Arc<RpcServerAsyncBridge>) {
    loop {
        let receive = {
            let Ok(state) = bridge.state.lock() else {
                return;
            };
            if state.stopped {
                return;
            }
            state.receive.clone().filter(|receive| receive.is_open())
        };
        // Probe the native request endpoint before constructing a WaitSet.
        // The request sidecar is advisory and may have coalesced a wake.
        if let Some(receive) = receive.clone() {
            let initial = match bridge.server.endpoint.lock() {
                Ok(mut endpoint) => match endpoint.as_mut() {
                    Some(server) => server.try_receive(),
                    None => Err(NativeRpcError::ActivationExpired),
                },
                Err(_) => Err(NativeRpcError::TransportFailure {
                    operation: "probe RPC server request",
                    detail: "RPC server endpoint lock was poisoned".to_owned(),
                }),
            };
            match initial {
                Ok(Some(request)) => {
                    receive.complete(ASYNC_COMPLETED, RpcServerAsyncOutcome::Request(request));
                    continue;
                }
                Ok(None) => {}
                Err(error) => {
                    let terminal = if matches!(error, NativeRpcError::ActivationExpired) {
                        ASYNC_EXPIRED
                    } else {
                        ASYNC_FAILED
                    };
                    receive.complete(terminal, RpcServerAsyncOutcome::Error(error));
                    continue;
                }
            }
        }
        let wait_activation = ActivationId::new(
            bridge
                .activation
                .value()
                .saturating_add(bridge.next_wait_activation.fetch_add(1, Ordering::Relaxed)),
        );
        let mut context = match WaitContext::new(bridge.process, wait_activation) {
            Ok(context) => context,
            Err(error) => {
                if let Some(receive) = receive {
                    receive.complete(
                        ASYNC_FAILED,
                        RpcServerAsyncOutcome::Error(NativeRpcError::TransportFailure {
                            operation: "create RPC asyncio WaitSet",
                            detail: error.to_string(),
                        }),
                    );
                }
                return;
            }
        };
        let wake = context.wake_handle();
        if let Ok(mut state) = bridge.state.lock() {
            state.wake = Some(wake);
            if state.stopped {
                continue;
            }
        }

        let endpoint = bridge.server.endpoint.lock().ok();
        let wait_result = if let Some(endpoint) = endpoint.as_ref() {
            if let Some(server) = endpoint.as_ref() {
                let source = server.request_event_source(WaitToken::new(1));
                context.run_with_timeout(&[source], Duration::from_millis(100), |event, _budget| {
                    if !event.is_synthetic() {
                        Ok::<WaitProgress, rbt_iox2::WaitError>(WaitProgress::Stop)
                    } else {
                        Ok::<WaitProgress, rbt_iox2::WaitError>(WaitProgress::Continue)
                    }
                })
            } else {
                Ok(None)
            }
        } else {
            Err(rbt_iox2::WaitError::ListenerWait(
                "RPC server endpoint lock was poisoned".to_owned(),
            ))
        };
        drop(endpoint);
        if let Err(error) = wait_result {
            if let Some(receive) = receive {
                receive.complete(
                    ASYNC_FAILED,
                    RpcServerAsyncOutcome::Error(NativeRpcError::TransportFailure {
                        operation: "run RPC asyncio WaitSet",
                        detail: error.to_string(),
                    }),
                );
            }
            return;
        }
        let Some(receive) = receive else {
            continue;
        };
        // The request sidecar is advisory. Probe the native RR endpoint on
        // every bounded wait return, including the safety timeout.
        let request = bridge.server.endpoint.lock().ok().and_then(|mut endpoint| {
            endpoint
                .as_mut()
                .and_then(|server| match server.try_receive() {
                    Ok(request) => request,
                    Err(error) => {
                        receive.complete(ASYNC_EXPIRED, RpcServerAsyncOutcome::Error(error));
                        None
                    }
                })
        });
        if let Some(request) = request {
            receive.complete(ASYNC_COMPLETED, RpcServerAsyncOutcome::Request(request));
            bridge.remove(&receive);
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcAsyncCall")]
pub(crate) struct PyRpcAsyncCall {
    state: Arc<RpcClientAsyncCallState>,
}

#[pymethods]
impl PyRpcAsyncCall {
    fn _cancel(&self) {
        self.state.cancel();
    }

    fn _deadline(&self) {
        self.state.deadline();
    }

    fn _result(&self) -> PyResult<PyRpcResponse> {
        self.state.take_outcome()
    }

    fn _done(&self) -> bool {
        !self.state.is_open()
    }

    fn _debug_terminal(&self) -> u8 {
        self.state.terminal()
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcAsyncReceive")]
pub(crate) struct PyRpcAsyncReceive {
    state: Arc<RpcServerAsyncReceiveState>,
}

#[pymethods]
impl PyRpcAsyncReceive {
    fn _cancel(&self) -> bool {
        self.state.cancel()
    }

    fn _result(&self) -> PyResult<PyRpcRequest> {
        self.state.take_outcome()
    }

    fn _done(&self) -> bool {
        !self.state.is_open()
    }

    fn _debug_terminal(&self) -> u8 {
        self.state.terminal.load(Ordering::Acquire)
    }
}

pub(crate) type RpcClientCell = Arc<Mutex<Option<RpcClientVariant>>>;
pub(crate) struct RpcServerCellState {
    pub(crate) endpoint: Mutex<Option<RpcServerVariant>>,
    pub(crate) wake: RpcServerWakeVariant,
    pub(crate) async_busy: std::sync::atomic::AtomicBool,
}

pub(crate) type RpcServerCell = Arc<RpcServerCellState>;

/// Exclusive ownership of one server receive operation.
///
/// The atomic claim is the synchronization point shared by synchronous
/// receive calls and the asynchronous bridge. Keeping the claim in a guard
/// makes every error, cancellation, and teardown path release it exactly once.
struct RpcReceiveOwnership {
    server: RpcServerCell,
}

impl Drop for RpcReceiveOwnership {
    fn drop(&mut self) {
        self.server.async_busy.store(false, Ordering::Release);
    }
}

impl RpcServerCellState {
    fn try_claim_receive(self: &RpcServerCell) -> Option<RpcReceiveOwnership> {
        self.async_busy
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .ok()
            .map(|_| RpcReceiveOwnership {
                server: Arc::clone(self),
            })
    }
}

fn server_cell(server: RpcServerVariant) -> RpcServerCell {
    let wake = server.wake_handle();
    Arc::new(RpcServerCellState {
        endpoint: Mutex::new(Some(server)),
        wake,
        async_busy: std::sync::atomic::AtomicBool::new(false),
    })
}

pub(crate) fn client_async_bridge(
    client: RpcClientCell,
    process: rbt_core::ProcessInstanceId,
    activation: ActivationId,
) -> Arc<RpcClientAsyncBridge> {
    RpcClientAsyncBridge::new(client, process, activation)
}

pub(crate) fn server_async_bridge(
    server: RpcServerCell,
    process: rbt_core::ProcessInstanceId,
    activation: ActivationId,
) -> Arc<RpcServerAsyncBridge> {
    RpcServerAsyncBridge::new(server, process, activation)
}

fn lock<'a, T>(value: &'a Mutex<T>, message: &'static str) -> PyResult<MutexGuard<'a, T>> {
    value.lock().map_err(|_| RbtError::new_err(message))
}

fn rpc_pressure_object<'py>(
    py: Python<'py>,
    pressure: rbt_core::RpcPressure,
) -> PyResult<Bound<'py, PyAny>> {
    let resources = PyDict::new(py);
    resources.set_item("max_in_flight", pressure.resources.max_in_flight)?;
    resources.set_item("max_clients", pressure.resources.max_clients)?;
    resources.set_item("max_request_loans", pressure.resources.max_request_loans)?;
    resources.set_item(
        "response_buffer_size",
        pressure.resources.response_buffer_size,
    )?;
    resources.set_item(
        "max_borrowed_responses",
        pressure.resources.max_borrowed_responses,
    )?;
    resources.set_item("max_response_loans", pressure.resources.max_response_loans)?;
    resources.set_item(
        "max_request_slice_len",
        pressure.resources.max_request_slice_len,
    )?;
    resources.set_item(
        "max_response_slice_len",
        pressure.resources.max_response_slice_len,
    )?;

    let client = pressure.client.map(|metrics| {
        let value = PyDict::new(py);
        let _ = value.set_item("calls_attempted", metrics.calls_attempted);
        let _ = value.set_item("calls_started", metrics.calls_started);
        let _ = value.set_item("calls_completed", metrics.calls_completed);
        let _ = value.set_item("calls_failed", metrics.calls_failed);
        let _ = value.set_item("calls_timed_out", metrics.calls_timed_out);
        let _ = value.set_item("calls_cancelled", metrics.calls_cancelled);
        let _ = value.set_item("calls_activation_expired", metrics.calls_activation_expired);
        let _ = value.set_item("in_flight_current", metrics.in_flight_current);
        let _ = value.set_item("in_flight_high_water", metrics.in_flight_high_water);
        let _ = value.set_item("response_guards_current", metrics.response_guards_current);
        let _ = value.set_item(
            "response_guards_high_water",
            metrics.response_guards_high_water,
        );
        let _ = value.set_item("capacity_rejections", metrics.capacity_rejections);
        let _ = value.set_item(
            "server_capacity_rejections",
            metrics.server_capacity_rejections,
        );
        let _ = value.set_item("server_unavailable", metrics.server_unavailable);
        let _ = value.set_item("request_loan_failures", metrics.request_loan_failures);
        let _ = value.set_item(
            "response_receive_failures",
            metrics.response_receive_failures,
        );
        let _ = value.set_item("call_duration_count", metrics.call_duration_count);
        let _ = value.set_item("call_duration_total_ns", metrics.call_duration_total_ns);
        let _ = value.set_item("call_duration_max_ns", metrics.call_duration_max_ns);
        value.into_any()
    });
    let server = pressure.server.map(|metrics| {
        let value = PyDict::new(py);
        let _ = value.set_item("requests_received", metrics.requests_received);
        let _ = value.set_item("requests_current", metrics.requests_current);
        let _ = value.set_item("requests_high_water", metrics.requests_high_water);
        let _ = value.set_item("responses_attempted", metrics.responses_attempted);
        let _ = value.set_item("responses_sent", metrics.responses_sent);
        let _ = value.set_item("responses_failed", metrics.responses_failed);
        let _ = value.set_item("responses_abandoned", metrics.responses_abandoned);
        let _ = value.set_item("request_borrow_failures", metrics.request_borrow_failures);
        let _ = value.set_item("response_loan_failures", metrics.response_loan_failures);
        value.into_any()
    });
    let result = PyDict::new(py);
    result.set_item("service_name", pressure.service_name)?;
    result.set_item(
        "capability_instance_id",
        pressure.capability_instance.map(|id| id.to_string()),
    )?;
    result.set_item("resources", resources)?;
    result.set_item(
        "client_activation",
        pressure.client_activation.map(|id| id.value()),
    )?;
    result.set_item("client", client)?;
    result.set_item("server", server)?;
    result.set_item(
        "snapshot_timestamp_unix_ms",
        pressure.snapshot_timestamp_unix_ms,
    )?;
    Ok(result.into_any())
}

pub(crate) enum RequestState {
    Active {
        request: Option<RpcRequestVariant>,
        address: usize,
        len: usize,
        borrowed: usize,
    },
    Consumed,
}

pub(crate) struct RequestShared {
    pub(crate) state: Mutex<RequestState>,
}

pub(crate) fn request_payload_parts(shared: &Arc<RequestShared>) -> PyResult<(usize, usize)> {
    let state = lock(&shared.state, "RPC request state lock was poisoned")?;
    match &*state {
        RequestState::Active { address, len, .. } => Ok((*address, *len)),
        RequestState::Consumed => Err(RbtError::new_err("RPC request was consumed")),
    }
}

pub(crate) fn request_image_header(shared: &Arc<RequestShared>) -> PyResult<ImageHeader> {
    let state = lock(&shared.state, "RPC request state lock was poisoned")?;
    match &*state {
        RequestState::Active {
            request: Some(request),
            ..
        } => request.image_header().ok_or_else(not_image_payload),
        RequestState::Active { request: None, .. } | RequestState::Consumed => {
            Err(RbtError::new_err("RPC request was consumed"))
        }
    }
}

fn request_response_is_slice(shared: &Arc<RequestShared>) -> PyResult<bool> {
    let state = lock(&shared.state, "RPC request state lock was poisoned")?;
    match &*state {
        RequestState::Active {
            request: Some(request),
            ..
        } => Ok(request.response_is_slice()),
        RequestState::Active { request: None, .. } | RequestState::Consumed => {
            Err(RbtError::new_err("RPC request was consumed"))
        }
    }
}

fn request_shared(request: RpcRequestVariant) -> Arc<RequestShared> {
    let (address, len) = request.parts();
    Arc::new(RequestShared {
        state: Mutex::new(RequestState::Active {
            request: Some(request),
            address,
            len,
            borrowed: 0,
        }),
    })
}

pub(crate) fn begin_request_borrow(shared: &Arc<RequestShared>) -> PyResult<(usize, usize)> {
    let mut state = lock(&shared.state, "RPC request state lock was poisoned")?;
    match &mut *state {
        &mut RequestState::Active {
            ref address,
            ref len,
            ref mut borrowed,
            ..
        } => {
            *borrowed = borrowed
                .checked_add(1)
                .ok_or_else(|| RbtError::new_err("RPC request borrow count overflow"))?;
            Ok((*address, *len))
        }
        RequestState::Consumed => Err(RbtError::new_err("RPC request has already been consumed")),
    }
}

pub(crate) fn end_request_borrow(shared: &Arc<RequestShared>) {
    if let Ok(mut state) = shared.state.lock()
        && let &mut RequestState::Active {
            ref mut borrowed, ..
        } = &mut *state
    {
        *borrowed = borrowed.saturating_sub(1);
    }
}

fn take_request_for_response(shared: &Arc<RequestShared>) -> PyResult<RpcRequestVariant> {
    let mut state = lock(&shared.state, "RPC request state lock was poisoned")?;
    let previous = std::mem::replace(&mut *state, RequestState::Consumed);
    match previous {
        RequestState::Active {
            request: Some(request),
            borrowed: 0,
            ..
        } => Ok(request),
        RequestState::Active {
            request,
            address,
            len,
            borrowed,
        } => {
            *state = RequestState::Active {
                request,
                address,
                len,
                borrowed,
            };
            Err(super::LoanStillMappedError::new_err(
                "RPC request still has live read-only mappings",
            ))
        }
        RequestState::Consumed => Err(RbtError::new_err("RPC request has already been consumed")),
    }
}

pub(crate) enum ResponseLoanState {
    Active {
        loan: Option<Box<RpcResponseLoanVariant>>,
        address: usize,
        len: usize,
        mappings: usize,
    },
    Consumed,
}

pub(crate) struct ResponseLoanShared {
    pub(crate) state: Mutex<ResponseLoanState>,
}

fn response_loan_shared(loan: RpcResponseLoanVariant) -> Arc<ResponseLoanShared> {
    let (address, len) = loan.parts();
    Arc::new(ResponseLoanShared {
        state: Mutex::new(ResponseLoanState::Active {
            loan: Some(Box::new(loan)),
            address,
            len,
            mappings: 0,
        }),
    })
}

pub(crate) fn create_response_lease(shared: &Arc<ResponseLoanShared>) -> PyResult<(usize, usize)> {
    let mut state = lock(&shared.state, "RPC response loan state lock was poisoned")?;
    match &mut *state {
        &mut ResponseLoanState::Active {
            ref address,
            ref len,
            ref mut mappings,
            ..
        } => {
            *mappings = mappings
                .checked_add(1)
                .ok_or_else(|| RbtError::new_err("RPC response mapping count overflow"))?;
            Ok((*address, *len))
        }
        ResponseLoanState::Consumed => Err(RbtError::new_err("RPC response loan was consumed")),
    }
}

pub(crate) fn decrement_response_lease(shared: &Arc<ResponseLoanShared>) {
    if let Ok(mut state) = shared.state.lock()
        && let &mut ResponseLoanState::Active {
            ref mut mappings, ..
        } = &mut *state
    {
        *mappings = mappings.saturating_sub(1);
    }
}

pub(crate) fn response_lease_is_active(shared: &Arc<ResponseLoanShared>) -> PyResult<()> {
    let state = lock(&shared.state, "RPC response loan state lock was poisoned")?;
    match &*state {
        ResponseLoanState::Active { mappings, .. } if *mappings > 0 => Ok(()),
        ResponseLoanState::Active { .. } => {
            Err(RbtError::new_err("RPC response lease is no longer mapped"))
        }
        ResponseLoanState::Consumed => Err(RbtError::new_err("RPC response loan was consumed")),
    }
}

fn take_response_for_send(shared: &Arc<ResponseLoanShared>) -> PyResult<RpcResponseLoanVariant> {
    let mut state = lock(&shared.state, "RPC response loan state lock was poisoned")?;
    if let ResponseLoanState::Active { mappings, .. } = &*state
        && *mappings != 0
    {
        return Err(super::LoanStillMappedError::new_err(
            "RPC response loan still has live writable mappings",
        ));
    }
    match std::mem::replace(&mut *state, ResponseLoanState::Consumed) {
        ResponseLoanState::Active {
            loan: Some(loan), ..
        } => Ok(*loan),
        ResponseLoanState::Active { loan: None, .. } | ResponseLoanState::Consumed => {
            Err(RbtError::new_err("RPC response loan was already consumed"))
        }
    }
}

pub(crate) enum RequestLoanState {
    Active {
        loan: Option<RpcRequestLoanVariant>,
        address: usize,
        len: usize,
        mappings: usize,
    },
    Consumed,
}

pub(crate) struct RequestLoanShared {
    pub(crate) state: Mutex<RequestLoanState>,
}

fn request_loan_shared(loan: RpcRequestLoanVariant) -> Arc<RequestLoanShared> {
    let (address, len) = loan.parts();
    Arc::new(RequestLoanShared {
        state: Mutex::new(RequestLoanState::Active {
            loan: Some(loan),
            address,
            len,
            mappings: 0,
        }),
    })
}

pub(crate) fn create_request_lease(shared: &Arc<RequestLoanShared>) -> PyResult<(usize, usize)> {
    let mut state = lock(&shared.state, "RPC request loan state lock was poisoned")?;
    match &mut *state {
        &mut RequestLoanState::Active {
            ref address,
            ref len,
            ref mut mappings,
            ..
        } => {
            *mappings = mappings
                .checked_add(1)
                .ok_or_else(|| RbtError::new_err("RPC request mapping count overflow"))?;
            Ok((*address, *len))
        }
        RequestLoanState::Consumed => Err(RbtError::new_err("RPC request loan was consumed")),
    }
}

pub(crate) fn decrement_request_lease(shared: &Arc<RequestLoanShared>) {
    if let Ok(mut state) = shared.state.lock()
        && let &mut RequestLoanState::Active {
            ref mut mappings, ..
        } = &mut *state
    {
        *mappings = mappings.saturating_sub(1);
    }
}

pub(crate) fn request_lease_is_active(shared: &Arc<RequestLoanShared>) -> PyResult<()> {
    let state = lock(&shared.state, "RPC request loan state lock was poisoned")?;
    match &*state {
        RequestLoanState::Active { mappings, .. } if *mappings > 0 => Ok(()),
        RequestLoanState::Active { .. } => {
            Err(RbtError::new_err("RPC request lease is no longer mapped"))
        }
        RequestLoanState::Consumed => Err(RbtError::new_err("RPC request loan was consumed")),
    }
}

fn take_request_for_send(shared: &Arc<RequestLoanShared>) -> PyResult<RpcRequestLoanVariant> {
    let mut state = lock(&shared.state, "RPC request loan state lock was poisoned")?;
    if let RequestLoanState::Active { mappings, .. } = &*state
        && *mappings != 0
    {
        return Err(super::LoanStillMappedError::new_err(
            "RPC request loan still has live writable mappings",
        ));
    }
    match std::mem::replace(&mut *state, RequestLoanState::Consumed) {
        RequestLoanState::Active {
            loan: Some(loan), ..
        } => Ok(loan),
        RequestLoanState::Active { loan: None, .. } | RequestLoanState::Consumed => {
            Err(RbtError::new_err("RPC request loan was already consumed"))
        }
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcClient")]
pub(crate) struct PyRpcClient {
    pub(crate) client: RpcClientCell,
    async_bridge: Arc<RpcClientAsyncBridge>,
}

impl PyRpcClient {
    pub(crate) fn from_activation(
        client: RpcClientCell,
        async_bridge: Arc<RpcClientAsyncBridge>,
    ) -> Self {
        Self {
            client,
            async_bridge,
        }
    }
}

#[pymethods]
impl PyRpcClient {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, capability_instance_id=None, activation_id=None))]
    fn open(
        descriptor_json: &str,
        capability_instance_id: Option<&str>,
        activation_id: Option<u64>,
    ) -> PyResult<Self> {
        let descriptor = rpc_descriptor(descriptor_json)?;
        let identity = match (capability_instance_id, activation_id) {
            (None, None) => None,
            (Some(capability), Some(activation)) => Some((
                activation_instance(capability)?,
                ActivationId::new(activation),
            )),
            _ => {
                return Err(PyValueError::new_err(
                    "capability_instance_id and activation_id must be supplied together",
                ));
            }
        };
        let client = RpcClientVariant::open(&descriptor, identity).map_err(rpc_py_error)?;
        let client = Arc::new(Mutex::new(Some(client)));
        let async_bridge = client_async_bridge(
            Arc::clone(&client),
            rbt_core::ProcessInstanceId::new(),
            activation_id
                .map(ActivationId::new)
                .unwrap_or_else(ActivationId::first),
        );
        Ok(Self {
            client,
            async_bridge,
        })
    }

    #[pyo3(signature = (element_count=None))]
    fn loan_request(&self, element_count: Option<usize>) -> PyResult<PyRpcRequestLoan> {
        let endpoint = lock(&self.client, "RPC client endpoint lock was poisoned")?;
        let endpoint = endpoint
            .as_ref()
            .ok_or_else(|| RpcActivationExpiredError::new_err("RPC client endpoint is closed"))?;
        match (endpoint.request_is_slice(), element_count) {
            (true, None) => {
                return Err(PyValueError::new_err(
                    "element_count is required for a dynamic RPC request",
                ));
            }
            (false, Some(_)) => {
                return Err(PyValueError::new_err(
                    "element_count is valid only for a dynamic RPC request",
                ));
            }
            _ => {}
        }
        let loan = endpoint.loan_request(element_count).map_err(rpc_py_error)?;
        Ok(PyRpcRequestLoan {
            shared: request_loan_shared(loan),
            async_bridge: Arc::clone(&self.async_bridge),
        })
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let endpoint = lock(&self.client, "RPC client endpoint lock was poisoned")?;
        let endpoint = endpoint
            .as_ref()
            .ok_or_else(|| RpcActivationExpiredError::new_err("RPC client endpoint is closed"))?;
        rpc_pressure_object(py, endpoint.pressure())
    }

    fn expire(&self, py: Python<'_>) -> PyResult<()> {
        let bridge = Arc::clone(&self.async_bridge);
        py.detach(move || bridge.expire_and_stop());
        Ok(())
    }

    fn activation_cancelled(&self) -> bool {
        self.client
            .lock()
            .ok()
            .and_then(|endpoint| {
                endpoint
                    .as_ref()
                    .map(|endpoint| endpoint.pressure().client.is_none())
            })
            .unwrap_or(true)
    }

    fn _debug_async_registration_count(&self) -> usize {
        self.async_bridge.pending_count()
    }
}

impl Drop for PyRpcClient {
    fn drop(&mut self) {
        let bridge = Arc::clone(&self.async_bridge);
        let client = Arc::clone(&self.client);
        Python::attach(|py| {
            py.detach(move || {
                bridge.expire_and_stop();
                close_client(&client);
            });
        });
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcRequestLoan")]
pub(crate) struct PyRpcRequestLoan {
    shared: Arc<RequestLoanShared>,
    async_bridge: Arc<RpcClientAsyncBridge>,
}

#[pymethods]
impl PyRpcRequestLoan {
    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC request loan state lock was poisoned",
        )?;
        match &*state {
            RequestLoanState::Active { len, .. } => Ok(*len),
            RequestLoanState::Consumed => Err(RbtError::new_err("RPC request loan was consumed")),
        }
    }

    fn _image_header(&self) -> PyResult<ImageHeaderTuple> {
        let state = lock(
            &self.shared.state,
            "RPC request loan state lock was poisoned",
        )?;
        match &*state {
            RequestLoanState::Active {
                loan: Some(loan), ..
            } => loan
                .image_header()
                .map(image_header_tuple)
                .ok_or_else(not_image_payload),
            RequestLoanState::Active { loan: None, .. } | RequestLoanState::Consumed => {
                Err(RbtError::new_err("RPC request loan was consumed"))
            }
        }
    }

    #[pyo3(signature = (*, sequence, timestamp_ns, width, height, stride, encoding))]
    fn _set_image_header(
        &self,
        sequence: u64,
        timestamp_ns: u64,
        width: u32,
        height: u32,
        stride: u32,
        encoding: u32,
    ) -> PyResult<()> {
        let mut state = lock(
            &self.shared.state,
            "RPC request loan state lock was poisoned",
        )?;
        match &mut *state {
            RequestLoanState::Active {
                loan: Some(loan), ..
            } => {
                if loan.set_image_header(make_image_header(
                    sequence,
                    timestamp_ns,
                    width,
                    height,
                    stride,
                    encoding,
                )) {
                    Ok(())
                } else {
                    Err(not_image_payload())
                }
            }
            RequestLoanState::Active { loan: None, .. } | RequestLoanState::Consumed => {
                Err(RbtError::new_err("RPC request loan was consumed"))
            }
        }
    }

    fn _new_lease(&self) -> PyResult<super::PyImageLease> {
        let (address, _) = create_request_lease(&self.shared)?;
        Ok(super::PyImageLease::from_rpc_request(
            Arc::clone(&self.shared),
            address,
        ))
    }

    #[pyo3(signature = (timeout_seconds=5.0))]
    fn send(&self, py: Python<'_>, timeout_seconds: f64) -> PyResult<PyRpcResponse> {
        let timeout = checked_rpc_timeout(timeout_seconds)?;
        let loan = take_request_for_send(&self.shared)?;
        let response = py
            .detach(move || loan.send(timeout))
            .map_err(rpc_py_error)?;
        Ok(PyRpcResponse {
            shared: response_guard_shared(response),
        })
    }

    fn send_async(
        &self,
        py: Python<'_>,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<PyRpcAsyncCall> {
        let loan = take_request_for_send(&self.shared)?;
        let state = RpcClientAsyncCallState::new(&self.async_bridge, loop_object, future);
        self.async_bridge.register(Arc::clone(&state))?;
        let pending = py.detach(move || loan.start());
        match pending {
            Ok(pending) => state.set_pending(pending),
            Err(error) => state.complete(ASYNC_FAILED, RpcClientAsyncOutcome::Error(error)),
        }
        Ok(PyRpcAsyncCall { state })
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC request loan state lock was poisoned",
        )?;
        match &*state {
            RequestLoanState::Active { address, .. } => Ok(*address),
            RequestLoanState::Consumed => Err(RbtError::new_err("RPC request loan was consumed")),
        }
    }

    fn _debug_export_count(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC request loan state lock was poisoned",
        )?;
        Ok(match &*state {
            RequestLoanState::Active { mappings, .. } => *mappings,
            RequestLoanState::Consumed => 0,
        })
    }
}

pub(crate) struct RpcResponseGuardShared {
    pub(crate) _response: Mutex<Option<RpcResponseGuardVariant>>,
    pub(crate) address: usize,
    pub(crate) len: usize,
}

fn response_guard_shared(response: RpcResponseGuardVariant) -> Arc<RpcResponseGuardShared> {
    let (address, len) = response.parts();
    Arc::new(RpcResponseGuardShared {
        _response: Mutex::new(Some(response)),
        address,
        len,
    })
}

pub(crate) fn response_guard_image_header(
    shared: &Arc<RpcResponseGuardShared>,
) -> PyResult<ImageHeader> {
    let response = lock(&shared._response, "RPC response guard lock was poisoned")?;
    response
        .as_ref()
        .ok_or_else(|| RbtError::new_err("RPC response guard was released"))?
        .image_header()
        .ok_or_else(not_image_payload)
}

#[pyclass(module = "rbt._rbt", name = "_RpcResponse")]
pub(crate) struct PyRpcResponse {
    shared: Arc<RpcResponseGuardShared>,
}

#[pymethods]
impl PyRpcResponse {
    #[getter]
    fn payload_nbytes(&self) -> usize {
        self.shared.len
    }

    fn _debug_payload_address(&self) -> usize {
        self.shared.address
    }

    fn _image_header(&self) -> PyResult<ImageHeaderTuple> {
        response_guard_image_header(&self.shared).map(image_header_tuple)
    }

    fn _new_lease(&self) -> PyResult<super::PyReceivedImageSample> {
        Ok(super::PyReceivedImageSample::from_rpc_response(Arc::clone(
            &self.shared,
        )))
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcServer")]
pub(crate) struct PyRpcServer {
    pub(crate) server: RpcServerCell,
    async_bridge: Arc<RpcServerAsyncBridge>,
}

impl PyRpcServer {
    pub(crate) fn from_activation(
        server: RpcServerCell,
        async_bridge: Arc<RpcServerAsyncBridge>,
    ) -> Self {
        Self {
            server,
            async_bridge,
        }
    }
}

#[pymethods]
impl PyRpcServer {
    #[staticmethod]
    #[pyo3(signature = (descriptor_json, capability_instance_id=None))]
    fn open(descriptor_json: &str, capability_instance_id: Option<&str>) -> PyResult<Self> {
        let descriptor = rpc_descriptor(descriptor_json)?;
        let capability = capability_instance_id
            .map(activation_instance)
            .transpose()?;
        let server = RpcServerVariant::open(&descriptor, capability).map_err(rpc_py_error)?;
        let server = server_cell(server);
        let async_bridge = server_async_bridge(
            Arc::clone(&server),
            rbt_core::ProcessInstanceId::new(),
            ActivationId::first(),
        );
        Ok(Self {
            server,
            async_bridge,
        })
    }

    fn try_receive(&self, py: Python<'_>) -> PyResult<Option<PyRpcRequest>> {
        let _ownership = self.server.try_claim_receive().ok_or_else(|| {
            crate::ConcurrentReceiveError::new_err(
                "receive conflicts with another synchronous or asynchronous receive",
            )
        })?;
        let cell = Arc::clone(&self.server);
        let request = py
            .detach(move || {
                let mut endpoint =
                    cell.endpoint
                        .lock()
                        .map_err(|_| NativeRpcError::TransportFailure {
                            operation: "receive RPC request",
                            detail: "RPC server endpoint lock was poisoned".to_owned(),
                        })?;
                endpoint
                    .as_mut()
                    .ok_or(NativeRpcError::ActivationExpired)?
                    .try_receive()
            })
            .map_err(rpc_py_error)?;
        Ok(request.map(|request| PyRpcRequest {
            shared: request_shared(request),
        }))
    }

    fn receive(
        &self,
        py: Python<'_>,
        timeout_seconds: Option<f64>,
    ) -> PyResult<Option<PyRpcRequest>> {
        let timeout = timeout_seconds.map(checked_rpc_timeout).transpose()?;
        let _ownership = self.server.try_claim_receive().ok_or_else(|| {
            crate::ConcurrentReceiveError::new_err(
                "receive conflicts with another synchronous or asynchronous receive",
            )
        })?;
        let cell = Arc::clone(&self.server);
        let request = py
            .detach(move || {
                let mut endpoint =
                    cell.endpoint
                        .lock()
                        .map_err(|_| NativeRpcError::TransportFailure {
                            operation: "receive RPC request",
                            detail: "RPC server endpoint lock was poisoned".to_owned(),
                        })?;
                let endpoint = endpoint.as_mut().ok_or(NativeRpcError::ActivationExpired)?;
                match timeout {
                    Some(timeout) => endpoint.receive_timeout(timeout),
                    None => endpoint.receive().map(Some),
                }
            })
            .map_err(rpc_py_error)?;
        Ok(request.map(|request| PyRpcRequest {
            shared: request_shared(request),
        }))
    }

    fn pressure<'py>(&self, py: Python<'py>) -> PyResult<Bound<'py, PyAny>> {
        let endpoint = lock(
            &self.server.endpoint,
            "RPC server endpoint lock was poisoned",
        )?;
        let endpoint = endpoint
            .as_ref()
            .ok_or_else(|| RpcActivationExpiredError::new_err("RPC server endpoint is closed"))?;
        rpc_pressure_object(py, endpoint.pressure())
    }

    fn expire(&self, py: Python<'_>) -> PyResult<()> {
        let bridge = Arc::clone(&self.async_bridge);
        let server = Arc::clone(&self.server);
        py.detach(move || {
            bridge.expire_and_stop();
            let _ = server.wake.expire();
        });
        Ok(())
    }

    fn begin_receive_async(
        &self,
        loop_object: Bound<'_, PyAny>,
        future: Bound<'_, PyAny>,
    ) -> PyResult<PyRpcAsyncReceive> {
        Ok(PyRpcAsyncReceive {
            state: self.async_bridge.register(loop_object, future)?,
        })
    }

    fn _debug_async_registration_count(&self) -> usize {
        self.async_bridge.pending_count()
    }
}

impl Drop for PyRpcServer {
    fn drop(&mut self) {
        let bridge = Arc::clone(&self.async_bridge);
        let server = Arc::clone(&self.server);
        Python::attach(|py| {
            py.detach(move || {
                bridge.expire_and_stop();
                close_server(&server);
            });
        });
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcRequest")]
pub(crate) struct PyRpcRequest {
    shared: Arc<RequestShared>,
}

#[pymethods]
impl PyRpcRequest {
    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock(&self.shared.state, "RPC request state lock was poisoned")?;
        match &*state {
            RequestState::Active { len, .. } => Ok(*len),
            RequestState::Consumed => Err(RbtError::new_err("RPC request was consumed")),
        }
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        Ok(request_payload_parts(&self.shared)?.0)
    }

    fn _image_header(&self) -> PyResult<ImageHeaderTuple> {
        request_image_header(&self.shared).map(image_header_tuple)
    }

    #[pyo3(signature = (element_count=None))]
    fn loan_response(&self, element_count: Option<usize>) -> PyResult<PyRpcResponseLoan> {
        match (request_response_is_slice(&self.shared)?, element_count) {
            (true, None) => {
                return Err(PyValueError::new_err(
                    "element_count is required for a dynamic RPC response",
                ));
            }
            (false, Some(_)) => {
                return Err(PyValueError::new_err(
                    "element_count is valid only for a dynamic RPC response",
                ));
            }
            _ => {}
        }
        let request = take_request_for_response(&self.shared)?;
        Ok(PyRpcResponseLoan {
            shared: response_loan_shared(
                request.loan_response(element_count).map_err(rpc_py_error)?,
            ),
        })
    }

    fn _new_lease(&self) -> PyResult<super::PyReceivedImageSample> {
        Ok(super::PyReceivedImageSample::from_rpc_request(Arc::clone(
            &self.shared,
        )))
    }

    #[pyo3(signature = (fill=None, element_count=None))]
    fn respond(
        &self,
        fill: Option<Bound<'_, PyAny>>,
        element_count: Option<usize>,
    ) -> PyResult<PyRpcResponseLoan> {
        let loan = self.loan_response(element_count)?;
        if let Some(fill) = fill {
            if !fill.is_callable() {
                return Err(PyValueError::new_err("response fill must be callable"));
            }
            let loan_object = Py::new(fill.py(), loan.clone())?;
            fill.call1((loan_object,))?;
        }
        Ok(loan)
    }
}

#[pyclass(module = "rbt._rbt", name = "_RpcResponseLoan", skip_from_py_object)]
pub(crate) struct PyRpcResponseLoan {
    shared: Arc<ResponseLoanShared>,
}

impl Clone for PyRpcResponseLoan {
    fn clone(&self) -> Self {
        Self {
            shared: Arc::clone(&self.shared),
        }
    }
}

#[pymethods]
impl PyRpcResponseLoan {
    #[getter]
    fn payload_nbytes(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC response loan state lock was poisoned",
        )?;
        match &*state {
            ResponseLoanState::Active { len, .. } => Ok(*len),
            ResponseLoanState::Consumed => Err(RbtError::new_err("RPC response loan was consumed")),
        }
    }

    fn _image_header(&self) -> PyResult<ImageHeaderTuple> {
        let state = lock(
            &self.shared.state,
            "RPC response loan state lock was poisoned",
        )?;
        match &*state {
            ResponseLoanState::Active {
                loan: Some(loan), ..
            } => loan
                .image_header()
                .map(image_header_tuple)
                .ok_or_else(not_image_payload),
            ResponseLoanState::Active { loan: None, .. } | ResponseLoanState::Consumed => {
                Err(RbtError::new_err("RPC response loan was consumed"))
            }
        }
    }

    #[pyo3(signature = (*, sequence, timestamp_ns, width, height, stride, encoding))]
    fn _set_image_header(
        &self,
        sequence: u64,
        timestamp_ns: u64,
        width: u32,
        height: u32,
        stride: u32,
        encoding: u32,
    ) -> PyResult<()> {
        let mut state = lock(
            &self.shared.state,
            "RPC response loan state lock was poisoned",
        )?;
        match &mut *state {
            ResponseLoanState::Active {
                loan: Some(loan), ..
            } => {
                if loan.set_image_header(make_image_header(
                    sequence,
                    timestamp_ns,
                    width,
                    height,
                    stride,
                    encoding,
                )) {
                    Ok(())
                } else {
                    Err(not_image_payload())
                }
            }
            ResponseLoanState::Active { loan: None, .. } | ResponseLoanState::Consumed => {
                Err(RbtError::new_err("RPC response loan was consumed"))
            }
        }
    }

    fn _new_lease(&self) -> PyResult<super::PyImageLease> {
        let (address, _) = create_response_lease(&self.shared)?;
        Ok(super::PyImageLease::from_rpc_response(
            Arc::clone(&self.shared),
            address,
        ))
    }

    fn send(&self) -> PyResult<()> {
        let loan = take_response_for_send(&self.shared)?;
        loan.send().map_err(rpc_py_error)
    }

    fn _debug_payload_address(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC response loan state lock was poisoned",
        )?;
        match &*state {
            ResponseLoanState::Active { address, .. } => Ok(*address),
            ResponseLoanState::Consumed => Err(RbtError::new_err("RPC response loan was consumed")),
        }
    }

    fn _debug_export_count(&self) -> PyResult<usize> {
        let state = lock(
            &self.shared.state,
            "RPC response loan state lock was poisoned",
        )?;
        Ok(match &*state {
            ResponseLoanState::Active { mappings, .. } => *mappings,
            ResponseLoanState::Consumed => 0,
        })
    }
}

pub(crate) fn close_client(cell: &RpcClientCell) {
    if let Ok(mut endpoint) = cell.lock()
        && let Some(endpoint) = endpoint.take()
    {
        let _ = endpoint.expire();
    }
}

pub(crate) fn close_server(cell: &RpcServerCell) {
    let _ = cell.wake.expire();
    if let Ok(mut endpoint) = cell.endpoint.lock()
        && let Some(endpoint) = endpoint.take()
    {
        drop(endpoint);
    }
}

pub(crate) fn open_client_for_activation(
    descriptor: &RpcDescriptor,
    capability_instance: CapabilityInstanceId,
    activation: ActivationId,
) -> PyResult<RpcClientCell> {
    let client = RpcClientVariant::open(descriptor, Some((capability_instance, activation)))
        .map_err(rpc_py_error)?;
    Ok(Arc::new(Mutex::new(Some(client))))
}

pub(crate) fn open_server_for_capability(
    descriptor: &RpcDescriptor,
    capability_instance: CapabilityInstanceId,
) -> PyResult<RpcServerCell> {
    let server =
        RpcServerVariant::open(descriptor, Some(capability_instance)).map_err(rpc_py_error)?;
    Ok(server_cell(server))
}

pub(crate) fn add_to_module(_py: Python<'_>, module: &Bound<'_, PyModule>) -> PyResult<()> {
    module.add_class::<PyRpcClient>()?;
    module.add_class::<PyRpcRequestLoan>()?;
    module.add_class::<PyRpcAsyncCall>()?;
    module.add_class::<PyRpcResponse>()?;
    module.add_class::<PyRpcServer>()?;
    module.add_class::<PyRpcRequest>()?;
    module.add_class::<PyRpcAsyncReceive>()?;
    module.add_class::<PyRpcResponseLoan>()?;
    Ok(())
}

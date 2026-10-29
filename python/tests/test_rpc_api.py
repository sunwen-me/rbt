from __future__ import annotations

import rbt
from rbt._generated.demo import FRAME_DESCRIPTOR


DESCRIPTOR = rbt.RpcDescriptor.fixed("test.rpc", FRAME_DESCRIPTOR, FRAME_DESCRIPTOR)


class _ResponseOwner:
    payload_nbytes = 16

    def _new_lease(self) -> bytes:
        return bytes(self.payload_nbytes)

    def _debug_payload_address(self) -> int:
        return 0x2000


class _RequestLoanOwner:
    payload_nbytes = 16

    def __init__(self) -> None:
        self.sent_timeout: float | None = None

    def _new_lease(self) -> bytearray:
        return bytearray(self.payload_nbytes)

    def send(self, timeout: float) -> _ResponseOwner:
        self.sent_timeout = timeout
        return _ResponseOwner()

    def _debug_payload_address(self) -> int:
        return 0x1000

    def _debug_export_count(self) -> int:
        return 0


class _ClientNative:
    def __init__(self) -> None:
        self.loan = _RequestLoanOwner()
        self.expired = False

    def loan_request(self) -> _RequestLoanOwner:
        return self.loan

    def pressure(self) -> dict[str, object]:
        return {"service_name": "test.rpc"}

    def expire(self) -> None:
        self.expired = True


class _RequestOwner:
    payload_nbytes = 16

    def _new_lease(self) -> bytes:
        return bytes(self.payload_nbytes)

    def loan_response(self) -> object:
        return _ResponseLoanOwner()


class _ResponseLoanOwner:
    payload_nbytes = 16

    def _new_lease(self) -> bytearray:
        return bytearray(self.payload_nbytes)

    def send(self) -> None:
        return None

    def _debug_payload_address(self) -> int:
        return 0x3000

    def _debug_export_count(self) -> int:
        return 0


class _ServerNative:
    def __init__(self) -> None:
        self.request = _RequestOwner()

    def try_receive(self) -> _RequestOwner:
        return self.request

    def receive(self, timeout: float | None) -> _RequestOwner:
        return self.request

    def pressure(self) -> dict[str, object]:
        return {"service_name": "test.rpc"}

    def expire(self) -> None:
        return None


def test_rpc_descriptor_uses_generated_message_metadata() -> None:
    roundtrip = rbt.RpcDescriptor.from_json(DESCRIPTOR.to_json())
    assert roundtrip.request_type.qualified_name == "rbt.demo/Frame@1"
    assert roundtrip.response_type.layout_hash == DESCRIPTOR.response_type.layout_hash
    assert roundtrip.transport.readiness is rbt.RpcReadinessProtocol.RPC_SIDECAR_V1


def test_client_call_and_payload_view_are_zero_copy_at_python_boundary() -> None:
    native = _ClientNative()
    client = rbt.RpcClient(native, DESCRIPTOR)

    def fill(loan: rbt.RpcRequestLoan) -> None:
        view = loan.payload_view()
        view[:4] = b"rbt!"

    response = client.call(fill, timeout=0.25)
    assert native.loan.sent_timeout == 0.25
    assert response.payload_view().readonly
    assert response._debug_payload_address() == 0x2000


def test_server_receive_keeps_request_read_only_and_response_writable() -> None:
    server = rbt.RpcServer(_ServerNative(), DESCRIPTOR)
    request = server.try_receive()
    assert request is not None
    assert request.payload_view().readonly
    response = request.respond(lambda loan: loan.payload_view().__setitem__(slice(0, 4), b"ok!!"))
    response.send()


def test_rpc_endpoint_contract_wraps_activation_metadata() -> None:
    native = _ClientNative()
    endpoint = (
        "rpc",
        native,
        DESCRIPTOR.to_json(),
        "provider-a",
        "00000000-0000-0000-0000-000000000001",
    )
    wrapped = rbt._subscriber_from_endpoint(endpoint, 7)
    assert isinstance(wrapped, rbt.RpcClient)
    assert wrapped.provider_component_id == "provider-a"
    assert wrapped.activation_id == 7

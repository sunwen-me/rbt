#include <rbt/control.hpp>
#include <rbt/stream.hpp>
#include <rbt/types.hpp>

#include <cstdint>
#include <cstdlib>

namespace {

constexpr rbt::LayoutHash kHash { 0x00U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U,
                                  0x88U, 0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU };

constexpr rbt::MessageTypeDescriptor kDescriptor {
    "rbt.test",
    "Frame",
    1U,
    rbt::MessageShape::Fixed,
    16U,
    8U,
    0U,
    0U,
    kHash,
    "sequence:u64@0;value:u64@8;padding:none",
};

void check(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void descriptor_equality_covers_complete_abi() {
    auto same = kDescriptor;
    check(rbt::descriptors_equal(kDescriptor, same));

    same.layout_hash[0] ^= 1U;
    check(!rbt::descriptors_equal(kDescriptor, same));
    same = kDescriptor;
    same.header_size += 1U;
    check(!rbt::descriptors_equal(kDescriptor, same));
    same = kDescriptor;
    same.fields = "different";
    check(!rbt::descriptors_equal(kDescriptor, same));
}

void preflight_happens_on_metadata_only() {
    const rbt::StreamDescriptor stream("unused/invalid/service",
                                       kDescriptor,
                                       rbt::StreamProfile::Sensor);
    check(!rbt::preflight(stream, kDescriptor).has_value());

    auto mismatch = kDescriptor;
    mismatch.name = "Other";
    const auto error = rbt::preflight(stream, mismatch);
    check(error.has_value());
    check(error.value() == rbt::StreamError::AbiMismatch);

    const rbt::StreamDescriptor legacy(
        "unused/invalid/service", kDescriptor, rbt::StreamProfile::Sensor, rbt::TransportFeatures {});
    const auto readiness_error = rbt::preflight(legacy, kDescriptor);
    check(readiness_error.has_value());
    check(readiness_error.value() == rbt::StreamError::MissingTransportFeature);
}

void generated_binding_rejects_disagreeing_expected_metadata() {
    auto expected = rbt::control::CONTROL_COMMAND_DESCRIPTOR;
    expected.layout_hash[0] ^= 1U;
    const rbt::StreamDescriptor stream(
        "unused/invalid/service", rbt::control::CONTROL_COMMAND_DESCRIPTOR, rbt::StreamProfile::Sensor);
    const auto result = rbt::FixedPublisher<rbt::control::ControlCommand>::create(stream, expected);
    check(!result.has_value());
    check(result.error() == rbt::StreamError::AbiMismatch);
}

void result_has_small_move_only_semantics() {
    rbt::Result<int> success(7);
    check(success.has_value());
    check(success.value() == 7);

    rbt::Result<int> failure(rbt::StreamError::Transport);
    check(!failure.has_value());
    check(failure.error() == rbt::StreamError::Transport);
}

} // namespace

int main() {
    static_assert(sizeof(rbt::LayoutHash) == 16U);
    check(rbt::lowercase_hex(kHash) == "00112233445566778899aabbccddeeff");
    descriptor_equality_covers_complete_abi();
    preflight_happens_on_metadata_only();
    generated_binding_rejects_disagreeing_expected_metadata();
    result_has_small_move_only_semantics();
    return 0;
}

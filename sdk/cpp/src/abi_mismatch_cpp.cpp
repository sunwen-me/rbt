#include <rbt/sensor.hpp>
#include <rbt/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

constexpr char kOriginalImageSignature[] =
    "header(sequence:u64@0,timestamp_ns:u64@8,width:u32@16,height:u32@20,stride:u32@24,encoding:PixelFormat@28,padding:none);element:u8@0";

std::uint8_t hex_digit(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<std::uint8_t>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<std::uint8_t>(value - 'A' + 10);
    }
    return 0xFFU;
}

bool parse_layout_hash(std::string_view text, rbt::LayoutHash& result) {
    if (text.size() != result.size() * 2U) {
        return false;
    }
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const auto high = hex_digit(text[index * 2U]);
        const auto low = hex_digit(text[index * 2U + 1U]);
        if (high > 0x0FU || low > 0x0FU) {
            return false;
        }
        result[index] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return true;
}

rbt::MessageTypeDescriptor original_image_descriptor(rbt::LayoutHash hash) {
    return rbt::MessageTypeDescriptor { "rbt.sensor",
                                        "Image",
                                        1U,
                                        rbt::MessageShape::Slice,
                                        32U,
                                        8U,
                                        1U,
                                        1U,
                                        hash,
                                        kOriginalImageSignature };
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: abi_mismatch_cpp SERVICE ACTUAL_HASH\n";
        return 2;
    }

    rbt::LayoutHash actual_hash {};
    if (!parse_layout_hash(argv[2], actual_hash)) {
        std::cerr << "ACTUAL_HASH must be exactly 32 hexadecimal characters\n";
        return 2;
    }

    const auto actual = original_image_descriptor(actual_hash);
    const rbt::StreamDescriptor stream(argv[1], actual, rbt::StreamProfile::Sensor);
    auto subscriber = rbt::SliceSubscriber<rbt::sensor::ImageHeader, std::uint8_t>::create(
        stream, rbt::sensor::IMAGE_DESCRIPTOR);
    if (subscriber || subscriber.error() != rbt::StreamError::AbiMismatch) {
        std::cerr << "ABI_MISMATCH expected AbiMismatch\n";
        return 1;
    }

    std::cout << "binding rejected: message=" << rbt::sensor::IMAGE_DESCRIPTOR.qualified_name()
              << " expected_layout=" << rbt::lowercase_hex(rbt::sensor::IMAGE_DESCRIPTOR.layout_hash)
              << " actual_layout=" << rbt::lowercase_hex(actual.layout_hash)
              << " expected_size=" << rbt::sensor::IMAGE_DESCRIPTOR.header_size
              << " actual_size=" << actual.header_size
              << " expected_align=" << rbt::sensor::IMAGE_DESCRIPTOR.header_align
              << " actual_align=" << actual.header_align
              << " error=AbiMismatch stage=preflight transport_opened=false\n";
    return 0;
}

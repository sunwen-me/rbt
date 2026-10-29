#ifndef RBT_CPP_VERSION_HPP
#define RBT_CPP_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace rbt {

/// Public Rust/C++ SDK release identifier.
inline constexpr std::string_view SDK_VERSION = "0.8.0";

/// Version of the source IDL language understood by rbt-idl.
inline constexpr std::uint16_t IDL_LANGUAGE_VERSION = 1U;
/// Version of the canonical message descriptor format.
inline constexpr std::uint16_t MESSAGE_DESCRIPTOR_VERSION = 1U;
/// Version of the TCP + NDJSON Composer control protocol.
inline constexpr std::uint16_t CONTROL_PROTOCOL_VERSION = 2U;
/// Version of the complete ComponentSnapshot representation.
inline constexpr std::uint16_t COMPOSER_SNAPSHOT_VERSION = 4U;

} // namespace rbt

#endif // RBT_CPP_VERSION_HPP

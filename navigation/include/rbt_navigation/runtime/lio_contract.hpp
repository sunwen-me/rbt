#pragma once
#include <rbt/navwire.hpp>

namespace rbt::navigation::lio {
// Semantic capability type, distinct from the generated wire type name.
struct LioNavigationFrameV1 {
    inline static constexpr char TYPE_NAME[] = "LioNavigationFrameV1";
};
} // namespace rbt::navigation::lio

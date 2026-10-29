#pragma once

#include <rbt_navigation/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rbt::navigation {

class GridMap;

struct FeasiblePathSegment {
    Path2D path;
    bool final_segment{true};
};

class FeasiblePathHandler final {
public:
    FeasiblePathHandler(bool enforce_path_inversion = true,
                        bool enforce_path_rotation = true,
                        double minimum_path_rotation = 0.01);

    [[nodiscard]] FeasiblePathSegment update(const Path2D& path,
                                             const Pose2D& robot_pose,
                                             std::uint64_t map_version,
                                             const GridMap* map_identity);
    void advance();

private:
    [[nodiscard]] std::size_t firstConstraint(std::size_t begin) const noexcept;
    void reset(const Path2D& path, std::uint64_t map_version,
               const GridMap* map_identity);

    bool enforce_path_inversion_;
    bool enforce_path_rotation_;
    double minimum_path_rotation_;
    bool initialized_{false};
    Path2D path_;
    std::uint64_t map_version_{};
    const GridMap* map_identity_{};
    std::size_t cursor_{};
    std::size_t progress_index_{};
    std::size_t current_boundary_{};
    bool current_final_segment_{true};
};

} // namespace rbt::navigation

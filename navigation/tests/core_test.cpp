#include <rbt_navigation/core/types.hpp>
#include <limits>
#include <iostream>
using namespace rbt::navigation;
int main() {
    if (!finite(Pose2D{}) || finite(Twist2D{std::numeric_limits<double>::quiet_NaN(),0})) return 1;
    if (!fresh(0,second,second) || fresh(second,0,second) || fresh(-1,0,second)) return 2;
    if (fresh(0,second+1,second) || MapSnapshot{}.valid()) return 3;
    if (std::abs(normalizeAngle(3*std::numbers::pi)+std::numbers::pi)>1e-10) return 4;
    if (!Path2D{}.poses.empty() || RobotState{}.localized || NavigationGoal{}.timeout_ns<=0) return 5;
    std::cout << "core domain, finite geometry and monotonic freshness: PASS\n";
}

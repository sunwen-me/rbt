#include <rbt_navigation/controller/dynamic_window.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>

namespace {

using rbt::navigation::DynamicWindowBounds;
using rbt::navigation::DynamicWindowLimits;
using rbt::navigation::Twist2D;
using rbt::navigation::computeDynamicWindow;
using rbt::navigation::regulateDynamicWindow;
using rbt::navigation::selectDynamicWindowVelocity;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool near(double actual, double expected, double tolerance = 1e-12) {
    return std::abs(actual - expected) <= tolerance;
}

template <typename Function>
void expectInvalid(Function&& function, const char* message) {
    bool threw = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, message);
}

void testDynamicWindowBounds() {
    DynamicWindowLimits limits;
    limits.max_linear = 2.0;
    limits.max_angular = 3.0;
    limits.linear_acceleration = 0.8;
    limits.linear_deceleration = 0.3;
    limits.angular_acceleration = 1.5;
    limits.angular_deceleration = 0.4;
    limits.allow_reverse = true;

    const DynamicWindowBounds positive = computeDynamicWindow({1.0, 0.8}, limits, 0.5);
    expect(near(positive.min_linear, 0.85) && near(positive.max_linear, 1.4),
        "positive motion must use positive acceleration and deceleration magnitudes");
    expect(near(positive.min_angular, 0.6) && near(positive.max_angular, 1.55),
        "positive angular motion must use its asymmetric limits");

    const DynamicWindowBounds negative = computeDynamicWindow({-1.0, -0.8}, limits, 0.5);
    expect(near(negative.min_linear, -1.4) && near(negative.max_linear, -0.85),
        "negative motion must accelerate away from zero and decelerate toward zero");
    expect(near(negative.min_angular, -1.55) && near(negative.max_angular, -0.6),
        "negative angular motion must use its asymmetric limits");

    limits.allow_reverse = false;
    const DynamicWindowBounds from_rest = computeDynamicWindow({0.0, 0.0}, limits, 0.5);
    expect(near(from_rest.min_linear, 0.0) && near(from_rest.max_linear, 0.4),
        "forward-only motion from rest must be clipped at zero");
    expect(near(from_rest.min_angular, -0.75) && near(from_rest.max_angular, 0.75),
        "near-zero angular motion must allow acceleration in both directions");

    limits.allow_reverse = true;
    limits.linear_acceleration = 2.0;
    limits.angular_acceleration = 2.0;
    const DynamicWindowBounds clipped = computeDynamicWindow({1.9, 2.95}, limits, 0.1);
    expect(near(clipped.min_linear, 1.87) && clipped.max_linear == limits.max_linear,
        "linear window must retain exact absolute speed limits");
    expect(near(clipped.min_angular, 2.91) && clipped.max_angular == limits.max_angular,
        "angular window must retain exact absolute speed limits");
}

void testRegulation() {
    const DynamicWindowBounds positive = regulateDynamicWindow({0.8, 1.2, -0.4, 0.5}, 0.3);
    expect(positive.min_linear == 0.8 && positive.max_linear == 0.8,
        "unreachable positive regulation must preserve the reachable deceleration boundary");
    expect(positive.min_angular == -0.4 && positive.max_angular == 0.5,
        "linear regulation must not change angular bounds");

    const DynamicWindowBounds negative = regulateDynamicWindow({-1.2, -0.8, -0.5, 0.6}, -0.3);
    expect(negative.min_linear == -0.8 && negative.max_linear == -0.8,
        "unreachable negative regulation must preserve the reachable deceleration boundary");

    const DynamicWindowBounds intersection = regulateDynamicWindow({-1.0, 1.0, -0.2, 0.2}, 0.4);
    expect(intersection.min_linear == 0.0 && intersection.max_linear == 0.4,
        "reachable speed regulation must intersect the signed target interval");
}

void testVelocitySelection() {
    const DynamicWindowBounds straight_window{-0.8, 1.2, -0.5, 0.5};
    const Twist2D straight_forward = selectDynamicWindowVelocity(straight_window, 0.0, 1.0);
    const Twist2D straight_reverse = selectDynamicWindowVelocity(straight_window, 0.0, -1.0);
    expect(straight_forward.linear_x == 1.2 && straight_forward.angular_z == 0.0,
        "straight forward selection must choose maximum linear speed");
    expect(straight_reverse.linear_x == -0.8 && straight_reverse.angular_z == 0.0,
        "straight reverse selection must choose minimum linear speed");

    const DynamicWindowBounds curved_window{-2.0, 2.0, -0.5, 0.5};
    const Twist2D curved_forward = selectDynamicWindowVelocity(curved_window, 2.0, 1.0);
    const Twist2D curved_reverse = selectDynamicWindowVelocity(curved_window, 2.0, -1.0);
    expect(near(curved_forward.linear_x, 0.25) && near(curved_forward.angular_z, 0.5),
        "curved forward selection must choose the furthest feasible line intersection");
    expect(near(curved_reverse.linear_x, -0.25) && near(curved_reverse.angular_z, -0.5),
        "curved reverse selection must choose the furthest feasible line intersection");

    const Twist2D no_intersection = selectDynamicWindowVelocity({1.0, 2.0, 0.0, 1.0}, -1.0, 1.0);
    expect(no_intersection.linear_x == 1.0 && no_intersection.angular_z == 0.0,
        "missing curved intersection must select the closest rectangle corner");

    const DynamicWindowBounds narrow_window{1.0, 1.0001, 0.0, 0.0};
    const Twist2D positive_tie = selectDynamicWindowVelocity(narrow_window, 1.0, 1.0);
    const Twist2D negative_tie = selectDynamicWindowVelocity(narrow_window, 1.0, -1.0);
    expect(positive_tie.linear_x == 1.0001 && negative_tie.linear_x == 1.0,
        "near-equal corner distances must prefer motion in the requested direction");

    const Twist2D under_curvature_epsilon =
        selectDynamicWindowVelocity({1.0, 2.0, 0.0004, 0.0005}, 0.000999, 1.0);
    expect(under_curvature_epsilon.linear_x == 2.0 && under_curvature_epsilon.angular_z == 0.0004,
        "curvature below the reference epsilon must use straight-line selection");
}

void testInvalidInputs() {
    DynamicWindowLimits limits;
    expectInvalid([&] { computeDynamicWindow({}, limits, 0.0); }, "zero dt must be rejected");
    expectInvalid([&] { computeDynamicWindow({}, limits, 1.001); }, "dt above one second must be rejected");
    expectInvalid([&] {
        computeDynamicWindow({}, limits, std::numeric_limits<double>::quiet_NaN());
    }, "nonfinite dt must be rejected");
    expectInvalid([&] {
        computeDynamicWindow({std::numeric_limits<double>::infinity(), 0.0}, limits, 0.1);
    }, "nonfinite previous commands must be rejected");
    expectInvalid([&] {
        computeDynamicWindow({-0.01, 0.0}, limits, 0.1);
    }, "forward-only mode must reject negative previous linear velocity");

    DynamicWindowLimits nonfinite_limits = limits;
    nonfinite_limits.max_angular = std::numeric_limits<double>::infinity();
    expectInvalid([&] { computeDynamicWindow({}, nonfinite_limits, 0.1); },
        "nonfinite configuration must be rejected");
    DynamicWindowLimits negative_deceleration = limits;
    negative_deceleration.linear_deceleration = -0.1;
    expectInvalid([&] { computeDynamicWindow({}, negative_deceleration, 0.1); },
        "deceleration configuration must be a positive magnitude");

    DynamicWindowLimits unreachable = limits;
    unreachable.max_linear = 2.0;
    unreachable.linear_deceleration = 0.1;
    expectInvalid([&] { computeDynamicWindow({3.0, 0.0}, unreachable, 0.1); },
        "empty intersection with absolute speed limits must be rejected");

    expectInvalid([&] {
        regulateDynamicWindow({1.0, 0.0, -1.0, 1.0}, 0.2);
    }, "empty regulation window must be rejected");
    expectInvalid([&] {
        regulateDynamicWindow({-1.0, 1.0, -1.0, 1.0},
            std::numeric_limits<double>::quiet_NaN());
    }, "nonfinite regulated velocity must be rejected");
    expectInvalid([&] {
        selectDynamicWindowVelocity({1.0, 0.0, -1.0, 1.0}, 1.0, 1.0);
    }, "empty velocity-selection window must be rejected");
    expectInvalid([&] {
        selectDynamicWindowVelocity({-1.0, 1.0, -1.0, 1.0},
            std::numeric_limits<double>::infinity(), 1.0);
    }, "nonfinite curvature must be rejected");
    expectInvalid([&] {
        selectDynamicWindowVelocity({-1.0, 1.0, -1.0, 1.0}, 1.0,
            std::numeric_limits<double>::quiet_NaN());
    }, "nonfinite direction sign must be rejected");
    expectInvalid([&] {
        selectDynamicWindowVelocity({-1.0, 1.0, -1.0, 1.0}, 1.0, 0.0);
    }, "zero direction sign must be rejected");
    expectInvalid([&] {
        selectDynamicWindowVelocity({-1.0, 1.0, -1.0, 1.0}, 1.0, 2.0);
    }, "direction signs other than plus or minus one must be rejected");
}

void testAgainstDenseOracle() {
    constexpr int kSamplesPerAxis = 81;
    constexpr int kWindowCount = 160;
    std::mt19937 generator(0xD7A9U);
    std::uniform_real_distribution<double> origin(-3.0, 3.0);
    std::uniform_real_distribution<double> width(0.05, 2.5);
    std::uniform_real_distribution<double> curvature_distribution(-5.0, 5.0);
    std::uniform_int_distribution<int> direction_distribution(0, 1);

    for (int window_index = 0; window_index < kWindowCount; ++window_index) {
        const double linear_origin = origin(generator);
        const double angular_origin = origin(generator);
        const DynamicWindowBounds window{
            linear_origin,
            linear_origin + width(generator),
            angular_origin,
            angular_origin + width(generator),
        };
        double curvature = curvature_distribution(generator);
        if (std::abs(curvature) < 0.01) {
            curvature = curvature < 0.0 ? -0.01 : 0.01;
        }
        const double sign = direction_distribution(generator) == 0 ? -1.0 : 1.0;
        const Twist2D selected = selectDynamicWindowVelocity(window, curvature, sign);

        expect(selected.linear_x >= window.min_linear && selected.linear_x <= window.max_linear &&
            selected.angular_z >= window.min_angular && selected.angular_z <= window.max_angular,
            "randomized selection must remain feasible");

        const double denominator = std::hypot(curvature, 1.0);
        const double selected_error =
            std::abs(curvature * selected.linear_x - selected.angular_z) / denominator;
        double sampled_best = std::numeric_limits<double>::infinity();
        for (int linear_index = 0; linear_index < kSamplesPerAxis; ++linear_index) {
            const double linear_fraction =
                static_cast<double>(linear_index) / static_cast<double>(kSamplesPerAxis - 1);
            const double linear = window.min_linear +
                (window.max_linear - window.min_linear) * linear_fraction;
            for (int angular_index = 0; angular_index < kSamplesPerAxis; ++angular_index) {
                const double angular_fraction =
                    static_cast<double>(angular_index) / static_cast<double>(kSamplesPerAxis - 1);
                const double angular = window.min_angular +
                    (window.max_angular - window.min_angular) * angular_fraction;
                const double sampled_error =
                    std::abs(curvature * linear - angular) / denominator;
                sampled_best = std::min(sampled_best, sampled_error);
            }
        }

        const double linear_step =
            (window.max_linear - window.min_linear) / static_cast<double>(kSamplesPerAxis - 1);
        const double angular_step =
            (window.max_angular - window.min_angular) / static_cast<double>(kSamplesPerAxis - 1);
        const double sampling_tolerance = 0.5 * std::hypot(linear_step, angular_step) + 1e-10;
        expect(selected_error <= sampled_best + sampling_tolerance,
            "analytic selection must match the dense numerical line-distance oracle");
    }
}

}  // namespace

int main() {
    try {
        testDynamicWindowBounds();
        testRegulation();
        testVelocitySelection();
        testInvalidInputs();
        testAgainstDenseOracle();
    } catch (const std::exception& error) {
        std::cerr << "dynamic window test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "dynamic window bounds, regulation, analytic selection, validation and dense oracle: PASS\n";
    return 0;
}

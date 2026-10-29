#include <rbt_dm_imu/config.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path write_config(std::string_view content) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
                      / ("rbt-dm-imu-config-" + std::to_string(nonce) + ".yaml");
    std::ofstream output(path);
    output << content;
    if (!output) {
        throw std::runtime_error("failed to write test config");
    }
    return path;
}

void test_default_and_override() {
    const auto defaults = rbt_dm_imu::load_config(std::nullopt);
    assert(defaults.publish_rate_hz == 200U);

    const auto path = write_config("publish_rate_hz: 75\n");
    const auto config = rbt_dm_imu::load_config(path.string());
    std::filesystem::remove(path);
    assert(config.publish_rate_hz == 75U);
}

void test_invalid_rate_is_rejected() {
    for (const auto rate : {0U, 1001U}) {
        auto config = rbt_dm_imu::Config {};
        config.publish_rate_hz = rate;
        bool rejected = false;
        try {
            rbt_dm_imu::validate_config(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
    }
}

void test_rate_gate_skips_missed_slots() {
    using Gate = rbt_dm_imu::PublishRateGate;
    const auto start = Gate::Clock::time_point {};
    Gate gate(200U, start);
    assert(gate.due(start));
    assert(!gate.due(start + std::chrono::milliseconds(4)));
    assert(gate.due(start + std::chrono::milliseconds(5)));
    assert(gate.due(start + std::chrono::milliseconds(17)));
    assert(!gate.due(start + std::chrono::milliseconds(19)));
    assert(gate.due(start + std::chrono::milliseconds(20)));
}

} // namespace

int main() {
    test_default_and_override();
    test_invalid_rate_is_rejected();
    test_rate_gate_skips_missed_slots();
}

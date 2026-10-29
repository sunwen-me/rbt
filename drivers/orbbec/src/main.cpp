#include <rbt_orbbec/component.hpp>

#include <rbt/version.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr std::string_view kDriverVersion = "0.2.0";

[[nodiscard]] std::string usage() {
    return "Usage:\n"
           "  rbt-orbbec-camera --help\n"
           "  rbt-orbbec-camera --version\n"
           "  rbt-orbbec-camera list\n"
           "  rbt-orbbec-camera profiles --serial SN\n"
           "  rbt-orbbec-camera filters --serial SN\n"
           "  rbt-orbbec-camera run --serial SN [--component-id ID] [--frame-prefix PREFIX]\n"
           "      [--width N] [--height N] [--fps N] [--left-ir] [--right-ir] [--points] [--imu]\n"
           "      [--no-color] [--no-depth] [--align-depth-to-color] [--depth-filter SPEC]\n"
           "      [--no-reconnect] [--reconnect-delay-ms N] [--frame-timeout-ms N] [--duration-sec N]\n";
}

[[nodiscard]] std::uint64_t parse_unsigned(std::string_view text, std::string_view option) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(option) + " requires an unsigned decimal value");
    }
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()) {
        throw std::invalid_argument(std::string(option) + " requires an unsigned decimal value");
    }
    return value;
}

[[nodiscard]] std::uint32_t parse_u32(std::string_view text, std::string_view option) {
    const auto value = parse_unsigned(text, option);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string(option) + " is larger than a 32-bit unsigned integer");
    }
    return static_cast<std::uint32_t>(value);
}

void print_devices_json() {
    const auto devices = rbt_orbbec::list_devices();
    bool has_unreadable_serial = false;
    std::cout << "{\"sdk_version\":" << rbt_orbbec::json_quote(rbt_orbbec::sdk_version())
              << ",\"devices\":[";
    for (std::size_t index = 0U; index < devices.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        const auto& device = devices[index];
        has_unreadable_serial = has_unreadable_serial || device.serial.empty();
        std::cout << "{\"name\":" << rbt_orbbec::json_quote(device.name)
                  << ",\"serial\":" << rbt_orbbec::json_quote(device.serial)
                  << ",\"firmware\":" << rbt_orbbec::json_quote(device.firmware)
                  << ",\"connection\":" << rbt_orbbec::json_quote(device.connection)
                  << ",\"product_id\":" << device.product_id << '}';
    }
    std::cout << "]}" << std::endl;
    if (has_unreadable_serial) {
        std::cerr << "Orbbec detected a device with an empty serial. Check read/write access to its matching "
                     "/dev/bus/usb node and Orbbec SDK logs, then rerun 'rbt-orbbec-camera list'."
                  << std::endl;
    }
}

[[nodiscard]] rbt_orbbec::Options parse_run_options(int argc, char** argv, int first_argument) {
    rbt_orbbec::Options options;
    bool serial_set = false;
    std::set<std::string_view> seen_options;
    for (int index = first_argument; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto mark_seen = [&] {
            if (!seen_options.insert(argument).second) {
                throw std::invalid_argument("duplicate run option: " + std::string(argument));
            }
        };
        const auto require_value = [&]() -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            ++index;
            const std::string_view value(argv[index]);
            if (value.size() >= 2U && value[0] == '-' && value[1] == '-') {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            return value;
        };

        if (argument == "--serial") {
            mark_seen();
            options.serial = require_value();
            serial_set = true;
        } else if (argument == "--component-id") {
            mark_seen();
            options.component_id = require_value();
        } else if (argument == "--frame-prefix") {
            mark_seen();
            options.frame_prefix = require_value();
        } else if (argument == "--width") {
            mark_seen();
            options.width = parse_u32(require_value(), argument);
        } else if (argument == "--height") {
            mark_seen();
            options.height = parse_u32(require_value(), argument);
        } else if (argument == "--fps") {
            mark_seen();
            options.fps = parse_u32(require_value(), argument);
        } else if (argument == "--frame-timeout-ms") {
            mark_seen();
            options.frame_timeout_ms = parse_u32(require_value(), argument);
        } else if (argument == "--duration-sec") {
            mark_seen();
            const auto duration = parse_unsigned(require_value(), argument);
            if (duration > 86400U) {
                throw std::invalid_argument("--duration-sec must be at most 86400 (omit it to run continuously)");
            }
            options.duration_seconds = duration;
        } else if (argument == "--left-ir") {
            mark_seen();
            options.left_ir = true;
        } else if (argument == "--right-ir") {
            mark_seen();
            options.right_ir = true;
        } else if (argument == "--points") {
            mark_seen();
            options.points = true;
        } else if (argument == "--imu") {
            mark_seen();
            options.imu = true;
        } else if (argument == "--no-color") {
            mark_seen();
            options.color = false;
        } else if (argument == "--no-depth") {
            mark_seen();
            options.depth = false;
        } else if (argument == "--align-depth-to-color") {
            mark_seen();
            options.align_depth_to_color = true;
        } else if (argument == "--depth-filter") {
            if (options.depth_filters.size() >= 16U) {
                throw std::invalid_argument("at most 16 --depth-filter options may be specified");
            }
            options.depth_filters.push_back(rbt_orbbec::parse_depth_filter(require_value()));
        } else if (argument == "--no-reconnect") {
            mark_seen();
            options.reconnect = false;
        } else if (argument == "--reconnect-delay-ms") {
            mark_seen();
            options.reconnect_delay_ms = parse_u32(require_value(), argument);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << usage();
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown run option: " + std::string(argument));
        }
    }
    if (!serial_set || options.serial.empty()) {
        throw std::invalid_argument("run requires an explicit --serial SN");
    }
    rbt_orbbec::validate_options(options);
    return options;
}

[[nodiscard]] int run_cli(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << usage();
        return 0;
    }

    const std::string_view command(argv[1]);
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << usage();
        return 0;
    }
    if (command == "--version") {
        std::cout << "rbt-orbbec-camera " << kDriverVersion << '\n'
                  << "RBT SDK " << rbt::SDK_VERSION << '\n'
                  << "Orbbec SDK " << rbt_orbbec::sdk_version() << '\n';
        return 0;
    }
    if (command == "list") {
        if (argc > 2) {
            if (argc == 3 && (std::string_view(argv[2]) == "--help" || std::string_view(argv[2]) == "-h")) {
                std::cout << usage();
                return 0;
            }
            throw std::invalid_argument("list does not accept options");
        }
        print_devices_json();
        return 0;
    }
    if (command == "profiles" || command == "filters") {
        std::string serial;
        bool serial_set = false;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--help" || argument == "-h") {
                std::cout << usage();
                return 0;
            }
            if (argument != "--serial") {
                throw std::invalid_argument("unknown " + std::string(command) + " option: "
                                            + std::string(argument));
            }
            if (serial_set) {
                throw std::invalid_argument("duplicate " + std::string(command) + " option: --serial");
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument("--serial requires a value");
            }
            serial = argv[++index];
            if (serial.size() >= 2U && serial[0] == '-' && serial[1] == '-') {
                throw std::invalid_argument("--serial requires a value");
            }
            serial_set = true;
        }
        if (serial.empty()) {
            throw std::invalid_argument(std::string(command) + " requires an explicit --serial SN");
        }
        if (command == "profiles") {
            std::cout << rbt_orbbec::describe_profiles(serial) << std::endl;
        } else {
            std::cout << rbt_orbbec::describe_filters(serial) << std::endl;
        }
        return 0;
    }
    if (command == "run") {
        auto options = parse_run_options(argc, argv, 2);
        return rbt_orbbec::run_camera_supervisor(std::move(options));
    }

    throw std::invalid_argument("unknown command: " + std::string(command));
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run_cli(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "rbt-orbbec-camera: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "rbt-orbbec-camera: unexpected failure\n";
        return 2;
    }
}

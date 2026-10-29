#include <rbt_navigation/map/map_io.hpp>

#include <png.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace rbt::navigation;

namespace {

class TempDirectory {
public:
    TempDirectory() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("rbt-map-io-" + std::to_string(stamp));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

void writeBinary(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create test file");
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output) throw std::runtime_error("cannot write test file");
}

void writeYaml(const std::filesystem::path& path, const std::string& image,
               const std::string& extra = "") {
    std::map<std::string, std::string> fields{
        {"image", image}, {"resolution", "0.25"}, {"origin", "[2.0, -3.0, 1.25]"},
        {"negate", "0"}, {"occupied_thresh", "0.65"}, {"free_thresh", "0.196"}};
    std::istringstream overrides(extra);
    std::string line;
    while (std::getline(overrides, line)) {
        if (line.empty()) continue;
        const std::size_t separator = line.find(':');
        if (separator == std::string::npos) throw std::runtime_error("invalid test YAML field");
        const std::size_t value_start = line.find_first_not_of(" \t", separator + 1);
        fields[line.substr(0, separator)] = value_start == std::string::npos
            ? std::string{} : line.substr(value_start);
    }
    std::ostringstream yaml;
    for (const auto& [key, value] : fields) yaml << key << ": " << value << "\n";
    writeBinary(path, yaml.str());
}

OccupancyGrid load(const std::filesystem::path& directory, const std::string& image,
                   const std::string& image_data, const std::string& extra = "",
                   std::size_t max_cells = 1'000'000) {
    writeBinary(directory / image, image_data);
    const auto yaml = directory / "map.yaml";
    writeYaml(yaml, image, extra);
    return loadOccupancyMap(yaml, max_cells);
}

template <typename Function>
void mustThrow(Function&& function) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

void writePng(const std::filesystem::path& path, std::size_t width, std::size_t height,
              const std::vector<std::uint8_t>& rgba) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = static_cast<png_uint_32>(width);
    image.height = static_cast<png_uint_32>(height);
    image.format = PNG_FORMAT_RGBA;
    if (!png_image_write_to_file(&image, path.c_str(), 0, rgba.data(), 0, nullptr)) {
        throw std::runtime_error("cannot create test PNG");
    }
}

std::string thresholdYaml(double free_threshold, double occupied_threshold,
                          const std::string& mode) {
    std::ostringstream yaml;
    yaml << std::setprecision(17)
         << "image: pixels.pgm\n"
         << "resolution: 1\n"
         << "origin: [0, 0, 0]\n"
         << "negate: true\n"
         << "free_thresh: " << free_threshold << "\n"
         << "occupied_thresh: " << occupied_threshold << "\n"
         << "mode: " << mode << "\n";
    return yaml.str();
}

} // namespace

int main() {
    {
        TempDirectory directory;
        const std::string pgm = "P2\r\n# top row\r\n2 2\r\n# max gray\r\n255\r\n"
                                "0 255\r\n255 0\r\n";
        const auto grid = load(directory.path(), "pixels.pgm", pgm);
        assert(grid.width == 2 && grid.height == 2);
        assert(grid.resolution == 0.25);
        assert(grid.origin.x == 2.0 && grid.origin.y == -3.0 && grid.origin.yaw == 1.25);
        assert(grid.occupied_threshold == 50);
        assert((grid.cells == std::vector<std::int8_t>{0, 100, 100, 0}));
    }
    {
        TempDirectory directory;
        std::string pgm = "P5\r\n# binary test\r\n2 2\r\n255\r\n";
        pgm.append("\x20\x23\x41\x64", 4); // Raster bytes are space, '#', 65, and 100.
        const auto grid = load(directory.path(), "pixels.pgm", pgm,
                               "mode: raw\nnegate: 1\n");
        assert((grid.cells == std::vector<std::int8_t>{65, 100, 32, 35}));
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P2\n5 1\n255\n63 64 128 191 192\n");
        const auto yaml = directory.path() / "map.yaml";
        const double free_threshold = 64.0 / 255.0;
        const double occupied_threshold = 192.0 / 255.0;
        writeBinary(yaml, thresholdYaml(free_threshold, occupied_threshold, "scale"));
        const auto scaled = loadOccupancyMap(yaml);
        assert((scaled.cells == std::vector<std::int8_t>{0, 0, 50, 99, 100}));
        writeBinary(yaml, thresholdYaml(free_threshold, occupied_threshold, "trinary"));
        const auto trinary = loadOccupancyMap(yaml);
        assert((trinary.cells == std::vector<std::int8_t>{0, 0, -1, -1, 100}));
    }
    {
        TempDirectory directory;
        std::string pgm = "P5\n3 1\n1000\n";
        pgm.append("\x01\xf4\x01\xf3\x03\xe8", 6); // 500, 499, 1000.
        const auto grid = load(directory.path(), "pixels.pgm", pgm,
            "mode: trinary\nnegate: true\nfree_thresh: 0.49\noccupied_thresh: 0.5\n");
        assert((grid.cells == std::vector<std::int8_t>{100, -1, 100}));
    }
    {
        TempDirectory directory;
        const auto image = directory.path() / "pixels.png";
        writePng(image, 3, 1, {255, 0, 0, 0, 255, 255, 255, 255,
                               0, 0, 255, 128});
        const auto yaml = directory.path() / "map.yaml";
        writeYaml(yaml, "pixels.png", "negate: true\nfree_thresh: 0.3\n"
                                     "occupied_thresh: 0.55\n");
        const auto grid = loadOccupancyMap(yaml);
        assert((grid.cells == std::vector<std::int8_t>{-1, 100, -1}));
        writeYaml(yaml, "pixels.png", "mode: raw\nnegate: 0\n");
        const auto raw = loadOccupancyMap(yaml);
        assert((raw.cells == std::vector<std::int8_t>{76, -1, 29}));
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P2\n1 1\n255\n128\n");
        const auto yaml = directory.path() / "map.yaml";
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        assert(loadOccupancyMap(yaml).cells[0] == -1); // mode defaults to trinary.
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P2\n1 1\n255\n90\n");
        const double double_occupancy = 1.0 - 90.0 / 255.0;
        const double occupied_threshold = std::nextafter(double_occupancy, 1.0);
        const float float_occupancy = 1.0F - 90.0F / 255.0F;
        assert(float_occupancy == static_cast<float>(occupied_threshold));
        std::ostringstream yaml_text;
        yaml_text << std::setprecision(17)
                  << "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                  << "negate: false\nfree_thresh: 0\noccupied_thresh: "
                  << occupied_threshold << "\nmode: trinary\n";
        const auto yaml = directory.path() / "map.yaml";
        writeBinary(yaml, yaml_text.str());
        assert(loadOccupancyMap(yaml).cells[0] == 100);

        const double collapsed_free = double_occupancy;
        const double collapsed_occupied = std::nextafter(collapsed_free, 1.0);
        assert(collapsed_free < collapsed_occupied);
        assert(static_cast<float>(collapsed_free) == static_cast<float>(collapsed_occupied));
        yaml_text.str("");
        yaml_text.clear();
        yaml_text << std::setprecision(17)
                  << "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                  << "negate: false\nfree_thresh: " << collapsed_free
                  << "\noccupied_thresh: " << collapsed_occupied << "\nmode: trinary\n";
        writeBinary(yaml, yaml_text.str());
        assert(loadOccupancyMap(yaml).cells[0] == 0);
    }
    {
        TempDirectory directory;
        const auto image = directory.path() / "pixels.png";
        writePng(image, 2, 2, {0, 0, 0, 255, 255, 255, 255, 255,
                               0, 0, 0, 255, 0, 0, 0, 255});
        const auto yaml = directory.path() / "map.yaml";
        writeYaml(yaml, "pixels.png");
        mustThrow([&] { (void)loadOccupancyMap(yaml, 3); });
        std::string bytes;
        {
            std::ifstream input(image, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        bytes.resize(bytes.size() / 2);
        writeBinary(image, bytes);
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P5\n2 2\n255\n\x00\x01");
        const auto yaml = directory.path() / "map.yaml";
        writeYaml(yaml, "pixels.pgm");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P2\n2 2\n255\n0 0 0 0\n");
        const auto yaml = directory.path() / "map.yaml";
        writeYaml(yaml, "pixels.pgm");
        mustThrow([&] { (void)loadOccupancyMap(yaml, 3); });
        mustThrow([&] { (void)loadOccupancyMap(yaml, 0); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: nan\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [nan, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: 2\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.2\nfree_thresh: 0.8\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: nan\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 1.1\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\nmode: gray\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n"
                          "resolution: 2\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        writeBinary(yaml, std::string((1U << 20) + 1, 'x'));
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
    }
    {
        TempDirectory directory;
        writeBinary(directory.path() / "pixels.pgm", "P2\n2 1\n255\n0 0\n");
        const auto yaml = directory.path() / "map.yaml";
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1.7e308\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(directory.path() / "pixels.pgm", "P2\n1 1\n255\n0\n");
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1.3e308\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
        writeBinary(yaml, "image: pixels.pgm\nresolution: 1e-323\norigin: [0, 0, 0]\n"
                          "negate: false\noccupied_thresh: 0.65\nfree_thresh: 0.196\n");
        mustThrow([&] { (void)loadOccupancyMap(yaml); });
    }
    {
        TempDirectory directory;
        std::string oversized = "P2\n1 1\n255\n0\n#";
        oversized.append(1U << 20, 'x');
        writeBinary(directory.path() / "pixels.pgm", oversized);
        const auto yaml = directory.path() / "map.yaml";
        writeYaml(yaml, "pixels.pgm");
        mustThrow([&] { (void)loadOccupancyMap(yaml, 1); });
    }

    std::cout << "map I/O tests passed\n";
}

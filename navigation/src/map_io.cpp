#include <rbt_navigation/map/map_io.hpp>

#include <png.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rbt::navigation {
namespace {

constexpr std::size_t kMaxYamlBytes = 1U << 20;
constexpr std::size_t kMaxImageBytes = 64U << 20;
constexpr std::size_t kImageHeaderAllowance = 1U << 20;

[[noreturn]] void invalid(const std::string& message) {
    throw std::runtime_error("invalid occupancy map: " + message);
}

std::size_t checkedImageByteLimit(std::size_t max_cells) {
    const std::size_t multiplier = 8;
    std::size_t scaled = kMaxImageBytes;
    if (max_cells <= (kMaxImageBytes - kImageHeaderAllowance) / multiplier) {
        scaled = max_cells * multiplier + kImageHeaderAllowance;
    }
    return std::max(kImageHeaderAllowance, std::min(scaled, kMaxImageBytes));
}

std::string readTextFile(const std::filesystem::path& path, std::size_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) invalid("cannot read " + path.string());
    if (size > limit) invalid("file is too large: " + path.string());

    std::ifstream input(path, std::ios::binary);
    if (!input) invalid("cannot open " + path.string());
    std::string contents(static_cast<std::size_t>(size), '\0');
    if (!contents.empty()) {
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(contents.size())) {
            invalid("cannot read complete file " + path.string());
        }
    }
    return contents;
}

double yamlDouble(const YAML::Node& node, const char* name) {
    if (!node || !node.IsScalar()) invalid(std::string("missing or invalid ") + name);
    double value{};
    try {
        value = node.as<double>();
    } catch (const YAML::Exception&) {
        invalid(std::string("invalid ") + name);
    }
    if (!std::isfinite(value)) invalid(std::string(name) + " must be finite");
    return value;
}

bool yamlBoolOr01(const YAML::Node& node, const char* name) {
    if (!node || !node.IsScalar()) invalid(std::string("missing or invalid ") + name);
    std::string value = node.Scalar();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (value == "true") return true;
    if (value == "false") return false;
    if (value == "0") return false;
    if (value == "1") return true;
    invalid(std::string(name) + " must be true, false, 0, or 1");
}

std::size_t parseSize(std::string_view value, const char* what) {
    std::size_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        result == 0) {
        invalid(std::string("invalid ") + what);
    }
    return result;
}

unsigned parseUnsigned(std::string_view value, const char* what) {
    unsigned result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        invalid(std::string("invalid ") + what);
    }
    return result;
}

struct Pixel {
    std::uint8_t gray{};
    std::uint8_t alpha{255};
};

struct Image {
    std::size_t width{};
    std::size_t height{};
    std::vector<Pixel> pixels;
};

class PngImage {
public:
    PngImage() {
        image_.version = PNG_IMAGE_VERSION;
    }
    ~PngImage() { png_image_free(&image_); }
    PngImage(const PngImage&) = delete;
    PngImage& operator=(const PngImage&) = delete;
    png_image* get() noexcept { return &image_; }
private:
    png_image image_{};
};

void checkDimensions(std::size_t width, std::size_t height, std::size_t max_cells) {
    if (width == 0 || height == 0 || width > std::numeric_limits<std::size_t>::max() / height) {
        invalid("image dimensions are invalid");
    }
    if (width * height > max_cells) invalid("image exceeds max_cells");
}

Image readPng(const std::filesystem::path& path, std::size_t max_cells) {
    PngImage png;
    if (!png_image_begin_read_from_file(png.get(), path.c_str())) {
        invalid("cannot read PNG " + path.string() + ": " + png.get()->message);
    }
    const std::size_t width = png.get()->width;
    const std::size_t height = png.get()->height;
    checkDimensions(width, height, max_cells);
    if (width > std::numeric_limits<std::size_t>::max() / 4) {
        invalid("PNG row is too large");
    }

    png.get()->format = PNG_FORMAT_RGBA;
    const std::size_t pixel_count = width * height;
    if (pixel_count > std::numeric_limits<std::size_t>::max() / 4) {
        invalid("PNG image is too large");
    }
    std::vector<png_byte> rgba(pixel_count * 4);
    if (!png_image_finish_read(png.get(), nullptr, rgba.data(), 0, nullptr)) {
        invalid("cannot decode complete PNG " + path.string() + ": " + png.get()->message);
    }

    Image result{width, height, std::vector<Pixel>(pixel_count)};
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const unsigned red = rgba[i * 4];
        const unsigned green = rgba[i * 4 + 1];
        const unsigned blue = rgba[i * 4 + 2];
        const unsigned luminance = (299U * red + 587U * green + 114U * blue + 500U) / 1000U;
        result.pixels[i] = {static_cast<std::uint8_t>(luminance), rgba[i * 4 + 3]};
    }
    return result;
}

class PnmTokens {
public:
    explicit PnmTokens(std::string_view input) : input_(input) {}

    std::string_view next() {
        skipWhitespaceAndComments();
        const std::size_t start = position_;
        while (position_ < input_.size() && !isWhitespace(input_[position_]) &&
               input_[position_] != '#') {
            ++position_;
        }
        if (position_ == start) invalid("malformed PGM header or raster");
        return input_.substr(start, position_ - start);
    }

    std::size_t position() const noexcept { return position_; }

    bool hasNext() {
        skipWhitespaceAndComments();
        return position_ < input_.size();
    }

    void consumeRasterSeparator() {
        if (position_ >= input_.size() || !isWhitespace(input_[position_])) {
            invalid("P5 header is missing its raster separator");
        }
        const char separator = input_[position_++];
        if (separator == '\r' && position_ < input_.size() && input_[position_] == '\n') {
            ++position_;
        }
    }

private:
    static bool isWhitespace(char value) noexcept {
        const auto byte = static_cast<unsigned char>(value);
        return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' ||
               byte == '\v' || byte == '\f';
    }

    void skipWhitespaceAndComments() {
        while (position_ < input_.size()) {
            if (isWhitespace(input_[position_])) {
                ++position_;
            } else if (input_[position_] == '#') {
                while (position_ < input_.size() && input_[position_] != '\n' &&
                       input_[position_] != '\r') {
                    ++position_;
                }
            } else {
                return;
            }
        }
    }

    std::string_view input_;
    std::size_t position_{};
};

std::uint8_t normalizeSample(unsigned sample, unsigned max_value) {
    return static_cast<std::uint8_t>((sample * 255U + max_value / 2U) / max_value);
}

Image readPgm(const std::string& content, std::size_t max_cells) {
    PnmTokens tokens(content);
    const std::string_view magic = tokens.next();
    if (magic != "P2" && magic != "P5") invalid("only P2 and P5 PGM images are supported");
    const std::size_t width = parseSize(tokens.next(), "PGM width");
    const std::size_t height = parseSize(tokens.next(), "PGM height");
    checkDimensions(width, height, max_cells);
    const unsigned max_value = parseUnsigned(tokens.next(), "PGM maxval");
    if (max_value == 0 || max_value > 65535) invalid("PGM maxval must be in 1..65535");

    const std::size_t count = width * height;
    Image result{width, height, std::vector<Pixel>(count)};
    if (magic == "P2") {
        for (std::size_t i = 0; i < count; ++i) {
            const unsigned sample = parseUnsigned(tokens.next(), "P2 pixel");
            if (sample > max_value) invalid("P2 pixel exceeds maxval");
            result.pixels[i].gray = normalizeSample(sample, max_value);
        }
        if (tokens.hasNext()) invalid("P2 image has more samples than its dimensions allow");
        return result;
    }

    tokens.consumeRasterSeparator();
    const std::size_t bytes_per_sample = max_value < 256 ? 1 : 2;
    if (count > std::numeric_limits<std::size_t>::max() / bytes_per_sample) {
        invalid("P5 raster is too large");
    }
    const std::size_t raster_bytes = count * bytes_per_sample;
    const std::size_t offset = tokens.position();
    if (offset > content.size() || raster_bytes > content.size() - offset) {
        invalid("truncated P5 raster");
    }
    for (std::size_t i = 0; i < count; ++i) {
        unsigned sample = static_cast<unsigned char>(content[offset + i * bytes_per_sample]);
        if (bytes_per_sample == 2) {
            sample = (sample << 8U) |
                     static_cast<unsigned char>(content[offset + i * 2 + 1]);
        }
        if (sample > max_value) invalid("P5 pixel exceeds maxval");
        result.pixels[i].gray = normalizeSample(sample, max_value);
    }
    return result;
}

Image readImage(const std::filesystem::path& path, std::size_t max_cells) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) invalid("cannot read image " + path.string());
    const std::size_t byte_limit = checkedImageByteLimit(max_cells);
    if (size > byte_limit) invalid("image file is too large: " + path.string());

    std::ifstream input(path, std::ios::binary);
    if (!input) invalid("cannot open image " + path.string());
    std::array<unsigned char, 8> signature{};
    input.read(reinterpret_cast<char*>(signature.data()),
               static_cast<std::streamsize>(signature.size()));
    const bool png_signature = input.gcount() == static_cast<std::streamsize>(signature.size()) &&
        png_sig_cmp(signature.data(), 0, signature.size()) == 0;
    input.close();
    if (png_signature) return readPng(path, max_cells);

    return readPgm(readTextFile(path, byte_limit), max_cells);
}

std::filesystem::path expandImagePath(std::string image) {
    if (image.empty()) invalid("image path is empty");
    if (image.size() >= 2 && image[0] == '~' && image[1] == '/') {
        if (const char* home = std::getenv("HOME"); home && *home) {
            image = std::string(home) + image.substr(1);
        }
    }
    return std::filesystem::path(image);
}

} // namespace

OccupancyGrid loadOccupancyMap(const std::filesystem::path& yaml_file, std::size_t max_cells) {
    const std::string yaml_text = readTextFile(yaml_file, kMaxYamlBytes);
    YAML::Node yaml;
    try {
        yaml = YAML::Load(yaml_text);
    } catch (const YAML::Exception& error) {
        invalid(std::string("cannot parse YAML: ") + error.what());
    }
    if (!yaml || !yaml.IsMap()) invalid("YAML root must be a mapping");

    constexpr std::array<std::string_view, 7> recognized_keys{
        "image", "resolution", "origin", "negate", "occupied_thresh", "free_thresh", "mode"};
    std::array<unsigned, recognized_keys.size()> key_counts{};
    for (const auto& entry : yaml) {
        if (!entry.first.IsScalar()) continue;
        const std::string_view key = entry.first.Scalar();
        const auto match = std::find(recognized_keys.begin(), recognized_keys.end(), key);
        if (match != recognized_keys.end() && ++key_counts[match - recognized_keys.begin()] > 1) {
            invalid("duplicate YAML key: " + std::string(key));
        }
    }

    const YAML::Node image_node = yaml["image"];
    if (!image_node || !image_node.IsScalar()) invalid("missing or invalid image");
    std::filesystem::path image_path = expandImagePath(image_node.Scalar());
    if (image_path.is_relative()) image_path = yaml_file.parent_path() / image_path;

    const double resolution = yamlDouble(yaml["resolution"], "resolution");
    if (resolution <= 0.0 || !std::isfinite(resolution * 0.25) || resolution * 0.25 == 0.0) {
        invalid("resolution must be positive and representable at quarter-cell scale");
    }
    const YAML::Node origin_node = yaml["origin"];
    if (!origin_node || !origin_node.IsSequence() || origin_node.size() != 3) {
        invalid("origin must contain x, y, and yaw");
    }
    Pose2D origin{yamlDouble(origin_node[0], "origin x"),
                  yamlDouble(origin_node[1], "origin y"),
                  yamlDouble(origin_node[2], "origin yaw")};

    const bool negate = yamlBoolOr01(yaml["negate"], "negate");
    const double occupied_threshold = yamlDouble(yaml["occupied_thresh"], "occupied_thresh");
    const double free_threshold = yamlDouble(yaml["free_thresh"], "free_thresh");
    if (free_threshold < 0.0 || occupied_threshold > 1.0 ||
        free_threshold >= occupied_threshold) {
        invalid("thresholds must satisfy 0 <= free_thresh < occupied_thresh <= 1");
    }

    std::string mode = "trinary";
    const YAML::Node mode_node = yaml["mode"];
    if (mode_node) {
        if (!mode_node.IsScalar()) invalid("mode must be a scalar");
        mode = mode_node.Scalar();
    }
    if (mode != "trinary" && mode != "scale" && mode != "raw") {
        invalid("mode must be trinary, scale, or raw");
    }

    Image image = readImage(image_path, max_cells);
    const double metric_width = static_cast<double>(image.width) * resolution;
    const double metric_height = static_cast<double>(image.height) * resolution;
    if (!std::isfinite(metric_width) || !std::isfinite(metric_height) ||
        !std::isfinite(std::hypot(metric_width, metric_height))) {
        invalid("map metric extent is not finite");
    }
    const double cosine = std::cos(origin.yaw);
    const double sine = std::sin(origin.yaw);
    const double corner_x[] = {
        origin.x, origin.x + cosine * metric_width,
        origin.x - sine * metric_height,
        origin.x + cosine * metric_width - sine * metric_height};
    const double corner_y[] = {
        origin.y, origin.y + sine * metric_width,
        origin.y + cosine * metric_height,
        origin.y + sine * metric_width + cosine * metric_height};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(corner_x[i]) || !std::isfinite(corner_y[i])) {
            invalid("map world extents are not finite");
        }
    }

    OccupancyGrid grid;
    grid.width = image.width;
    grid.height = image.height;
    grid.resolution = resolution;
    grid.origin = origin;
    grid.occupied_threshold = 50;
    grid.cells.resize(image.pixels.size());
    for (std::size_t source_y = 0; source_y < image.height; ++source_y) {
        const std::size_t grid_y = image.height - 1 - source_y;
        for (std::size_t x = 0; x < image.width; ++x) {
            const Pixel pixel = image.pixels[source_y * image.width + x];
            std::int8_t value = -1;
            if (mode == "raw") {
                if (pixel.gray <= 100) value = static_cast<std::int8_t>(pixel.gray);
            } else if (pixel.alpha == 255) {
                float occupancy = static_cast<float>(pixel.gray) / 255.0F;
                if (!negate) occupancy = 1.0F - occupancy;
                const float occupied = static_cast<float>(occupied_threshold);
                const float free = static_cast<float>(free_threshold);
                if (occupancy <= free) {
                    value = 0;
                } else if (occupancy >= occupied) {
                    value = 100;
                } else if (mode == "scale") {
                    const float span = static_cast<float>(occupied_threshold - free_threshold);
                    const float scaled = (occupancy - free) / span * 100.0F;
                    value = static_cast<std::int8_t>(std::round(std::clamp(scaled, 0.0F, 100.0F)));
                }
            }
            grid.cells[grid_y * image.width + x] = value;
        }
    }
    return grid;
}

} // namespace rbt::navigation

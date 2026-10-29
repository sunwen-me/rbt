#include <rbt/control.hpp>
#include <rbt/sensor.hpp>
#include <rbt/types.hpp>

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <utility>

namespace {

template <typename T>
void print_layout(const char* type_name,
                  std::initializer_list<std::pair<const char*, std::size_t>> offsets) {
    std::cout << "LAYOUT type=" << type_name << " size=" << sizeof(T) << " align=" << alignof(T);
    for (const auto& [field_name, offset] : offsets) {
        std::cout << ' ' << field_name << "_offset=" << offset;
    }
    std::cout << '\n';
}

void print_descriptor(const char* type_name, const rbt::MessageTypeDescriptor& descriptor) {
    std::cout << "DESCRIPTOR type=" << type_name << " namespace=" << descriptor.package
              << " name=" << descriptor.name << " abi_major=" << descriptor.abi_major << " shape="
              << (descriptor.shape == rbt::MessageShape::Fixed ? "fixed" : "slice")
              << " header_size=" << descriptor.header_size << " header_align=" << descriptor.header_align
              << " element_size=" << descriptor.element_size << " element_align=" << descriptor.element_align
              << " layout_hash=" << rbt::lowercase_hex(descriptor.layout_hash) << '\n';
}

} // namespace

int main() {
    print_layout<rbt::sensor::ImageHeader>(
        "ImageHeader",
        {{"sequence", offsetof(rbt::sensor::ImageHeader, sequence)},
         {"timestamp_ns", offsetof(rbt::sensor::ImageHeader, timestamp_ns)},
         {"width", offsetof(rbt::sensor::ImageHeader, width)},
         {"height", offsetof(rbt::sensor::ImageHeader, height)},
         {"stride", offsetof(rbt::sensor::ImageHeader, stride)},
         {"encoding", offsetof(rbt::sensor::ImageHeader, encoding)}});
    print_layout<rbt::sensor::PointCloudHeader>(
        "PointCloudHeader",
        {{"sequence", offsetof(rbt::sensor::PointCloudHeader, sequence)},
         {"timestamp_ns", offsetof(rbt::sensor::PointCloudHeader, timestamp_ns)},
         {"width", offsetof(rbt::sensor::PointCloudHeader, width)},
         {"height", offsetof(rbt::sensor::PointCloudHeader, height)},
         {"dense", offsetof(rbt::sensor::PointCloudHeader, dense)},
         {"reserved", offsetof(rbt::sensor::PointCloudHeader, reserved)}});
    print_layout<rbt::sensor::PointXYZI>(
        "PointXYZI",
        {{"x", offsetof(rbt::sensor::PointXYZI, x)},
         {"y", offsetof(rbt::sensor::PointXYZI, y)},
         {"z", offsetof(rbt::sensor::PointXYZI, z)},
         {"intensity", offsetof(rbt::sensor::PointXYZI, intensity)}});
    print_layout<rbt::control::ControlCommand>(
        "ControlCommand",
        {{"sequence", offsetof(rbt::control::ControlCommand, sequence)},
         {"value", offsetof(rbt::control::ControlCommand, value)},
         {"reserved", offsetof(rbt::control::ControlCommand, reserved)}});

    print_descriptor("Image", rbt::sensor::IMAGE_DESCRIPTOR);
    print_descriptor("PointCloud", rbt::sensor::POINT_CLOUD_DESCRIPTOR);
    print_descriptor("ControlCommand", rbt::control::CONTROL_COMMAND_DESCRIPTOR);
    return 0;
}

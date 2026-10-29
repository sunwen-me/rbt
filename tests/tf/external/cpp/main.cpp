#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt/transforms.hpp>
#include <rbt/tf_transport.hpp>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

int main() {
    using namespace rbt::tf;

    std::array<std::uint8_t, 48U> source_bytes {};
    source_bytes[0] = 1U;
    const auto source = rbt::time::ClockSourceId(source_bytes);
    const auto clock_result = rbt::time::ClockIdentity::create(
        rbt::time::ClockKind::Device,
        source,
        7U
    );
    assert(clock_result);
    const auto stamp = rbt::time::Timestamp { clock_result.value(), 42 };

    TransformBuffer buffer;
    const auto authority = buffer.bind_provider(
        "external",
        "00112233-4455-6677-8899-aabbccddeeff"
    );
    buffer.apply_static_snapshot(
        authority,
        StaticSet {
            1U,
            stamp,
            0U,
            { StaticEdge {
                FrameId("world"),
                FrameId("base"),
                Transform3(Vec3(1.0, 2.0, 3.0), Quaternion::identity()),
            } },
        }
    );
    const auto result = buffer.lookup_at(
        "world",
        "base",
        stamp,
        LookupOptions { 0U, {} }
    );
    assert(result.transform.translation().x() == 1.0);
    assert(buffer.pressure_snapshot().active_providers == 1U);

    buffer.unbind_provider(authority);
    const auto after_unbind = buffer.pressure_snapshot();
    assert(after_unbind.active_providers == 0U);
    assert(after_unbind.graph.empty());

    const rbt::CapabilityInstanceId native_authority = "ffeeddcc-bbaa-9988-7766-554433221100";
    rbt::tf::TransformBuffer native_buffer;
    auto native_publisher = rbt::tf::DynamicTfPublisher::open(native_authority);
    auto native_subscriber = rbt::tf::DynamicTfSubscriber::open(
        native_authority,
        "external_native",
        native_buffer
    );
    native_publisher.publish(rbt::tf::DynamicRecord {
        rbt::tf::FrameId("native_world"),
        rbt::tf::FrameId("native_base"),
        stamp,
        0U,
        1U,
        rbt::tf::Transform3(rbt::tf::Vec3(4.0, 5.0, 6.0), rbt::tf::Quaternion::identity()),
    });
    assert(native_subscriber.drain() == 1U);
    const auto native_result = native_buffer.lookup_at(
        "native_world",
        "native_base",
        stamp,
        rbt::tf::LookupOptions { 0U, {} }
    );
    assert(native_result.transform.translation().x() == 4.0);
    return 0;
}

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rbt_orbbec/detail/imu_queue.hpp>
#include <cassert>
#include <future>
#include <limits>
#include <thread>

using namespace rbt_orbbec;
using namespace rbt_orbbec::detail;
using namespace std::chrono_literals;

int main() {
    ImuQueue fifo;
    for (std::uint64_t i = 0; i < 130; ++i)
        fifo.push({i % 2 ? Kind::Gyro : Kind::Accel, i, i * 5000, {1.25F, -2.5F, 3.75F}});
    std::uint64_t expected = 0;
    while (expected < 130) {
        auto batch = fifo.wait_and_drain(0ms);
        assert(batch.size && batch.size <= ImuQueue::kMaximumBatchSize);
        for (std::size_t i = 0; i < batch.size; ++i) {
            const auto& sample = batch.samples[i];
            assert(sample.index == expected && sample.capture_us == expected * 5000);
            assert(sample.kind == (expected % 2 ? Kind::Gyro : Kind::Accel));
            assert(sample.xyz[0] == 1.25F && sample.xyz[1] == -2.5F && sample.xyz[2] == 3.75F);
            ++expected;
        }
    }
    assert(fifo.wait_and_drain(0ms).size == 0);
    // Wraparound retains order and does not retain SDK frame owners.
    for (std::uint64_t i = 130; i < 330; ++i) fifo.push({Kind::Accel, i, i, {0, 0, 1}});
    while (expected < 330) {
        auto batch = fifo.wait_and_drain(0ms);
        for (std::size_t i = 0; i < batch.size; ++i) assert(batch.samples[i].index == expected++);
    }
    ImuQueue full;
    for (std::size_t i = 0; i < ImuQueue::kCapacity; ++i) full.push({Kind::Accel, i, i, {0, 0, 1}});
    bool overflow = false;
    try { full.push({Kind::Gyro, 999, 1, {0, 0, 1}}); } catch (const std::overflow_error&) { overflow = true; }
    assert(overflow);
    try { (void)full.wait_and_drain(0ms); assert(false); } catch (const std::overflow_error&) {}
    ImuQueue failed;
    failed.fail(std::make_exception_ptr(std::runtime_error("SDK callback error")));
    try { (void)failed.wait_and_drain(0ms); assert(false); }
    catch (const std::runtime_error& e) { assert(std::string(e.what()) == "SDK callback error"); }
    bool invalid = false;
    try { fifo.push({Kind::Accel, 1, 1, {std::numeric_limits<float>::quiet_NaN(), 0, 1}}); }
    catch (const std::runtime_error&) { invalid = true; }
    assert(invalid);
    ImuQueue closing;
    auto waiting = std::async(std::launch::async, [&] { return closing.wait_and_drain(2s).size; });
    std::this_thread::sleep_for(5ms);
    closing.close();
    assert(waiting.wait_for(100ms) == std::future_status::ready && waiting.get() == 0);
    closing.push({Kind::Accel, 1, 1, {0, 0, 1}});
    assert(closing.wait_and_drain(0ms).size == 0);
}

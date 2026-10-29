#include <rbt_orbbec/driver.hpp>

#include <libobsensor/ObSensor.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace rbt_orbbec {
namespace {

struct MonitorWakeState {
    void notify() noexcept {
        try {
            std::lock_guard lock(mutex);
            if (epoch != std::numeric_limits<std::uint64_t>::max()) {
                ++epoch;
            }
        } catch (...) {
        }
        condition.notify_all();
    }

    void record_selected_removal() noexcept {
        auto current = disconnect_generation.load(std::memory_order_relaxed);
        while (current != std::numeric_limits<std::uint64_t>::max()
               && !disconnect_generation.compare_exchange_weak(
                   current, current + 1U, std::memory_order_release, std::memory_order_relaxed)) {
        }
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t epoch { 0U };
    std::atomic<std::uint64_t> disconnect_generation { 0U };
};

class OrbbecDeviceMonitor final : public DeviceMonitor {
  public:
    explicit OrbbecDeviceMonitor(std::string serial)
        : context_(std::make_shared<ob::Context>()), serial_(std::move(serial)), wake_(std::make_shared<MonitorWakeState>()) {
        const std::weak_ptr<MonitorWakeState> weak_wake(wake_);
        const std::string selected_serial = serial_;
        callback_id_ = context_->registerDeviceChangedCallback(
            [weak_wake, selected_serial](std::shared_ptr<ob::DeviceList> removed,
                                         std::shared_ptr<ob::DeviceList>) noexcept {
                const auto wake = weak_wake.lock();
                if (!wake) {
                    return;
                }
                try {
                    if (removed) {
                        for (std::uint32_t index = 0U; index < removed->getCount(); ++index) {
                            const char* serial = removed->getSerialNumber(index);
                            if (serial != nullptr && selected_serial == serial) {
                                wake->record_selected_removal();
                                break;
                            }
                        }
                    }
                } catch (...) {
                    // A failed metadata read must not escape through the SDK
                    // callback; connected() provides the bounded fallback.
                }
                wake->notify();
            });
    }

    ~OrbbecDeviceMonitor() override {
        if (!callback_id_) {
            return;
        }
        try {
            context_->unregisterDeviceChangedCallback(*callback_id_);
        } catch (...) {
        }
    }

    [[nodiscard]] bool connected() override {
        const auto devices = context_->queryDeviceList();
        for (std::uint32_t index = 0U; index < devices->getCount(); ++index) {
            const char* serial = devices->getSerialNumber(index);
            if (serial != nullptr && serial_ == serial) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::uint64_t disconnect_generation() const noexcept override {
        return wake_->disconnect_generation.load(std::memory_order_acquire);
    }

    void wait_for_change(std::chrono::milliseconds timeout) override {
        if (timeout <= std::chrono::milliseconds::zero()) {
            return;
        }
        std::unique_lock lock(wake_->mutex);
        const auto observed = wake_->epoch;
        (void)wake_->condition.wait_for(lock, timeout, [&] { return wake_->epoch != observed; });
    }

  private:
    std::shared_ptr<ob::Context> context_;
    std::string serial_;
    std::shared_ptr<MonitorWakeState> wake_;
    std::optional<OBCallbackId> callback_id_;
};

} // namespace

std::unique_ptr<DeviceMonitor> watch_device(const std::string& serial) {
    if (serial.empty()) {
        throw std::invalid_argument("an explicit device serial is required for hotplug monitoring");
    }
    return std::make_unique<OrbbecDeviceMonitor>(serial);
}

} // namespace rbt_orbbec

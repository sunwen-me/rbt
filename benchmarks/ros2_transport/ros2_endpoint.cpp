#include "common.hpp"

#include <transport_bench/msg/cloud_frame.hpp>
#include <transport_bench/msg/imu_frame.hpp>
#include <transport_bench/msg/video_frame.hpp>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/publisher/DataWriter.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/Subscriber.hpp>
#if __has_include(<fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>)
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.hpp>
#else
// Fast DDS 2.x (ROS 2 Humble) exports the same descriptor in a .h header.
#include <fastdds/rtps/transport/shared_mem/SharedMemTransportDescriptor.h>
#endif
#include <rcl/error_handling.h>
#include <rcl/publisher.h>
#include <rcl/subscription.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/get_message_type_support_handle.hpp>
#include <rmw/rmw.h>
#include <rmw_fastrtps_shared_cpp/custom_publisher_info.hpp>
#include <rmw_fastrtps_shared_cpp/custom_subscriber_info.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <typeinfo>

namespace {

using namespace transport_bench;
using ImuFrame = transport_bench::msg::ImuFrame;
using CloudFrame = transport_bench::msg::CloudFrame;
using VideoFrame = transport_bench::msg::VideoFrame;

std::string error_string(const char* operation, rcl_ret_t result) {
    const auto error = rcl_get_error_string();
    const char* detail = error.str;
    std::string message(operation);
    message += " failed with RCL code ";
    message += std::to_string(result);
    if (detail != nullptr && detail[0] != '\0') {
        message += ": ";
        message += detail;
    }
    rcl_reset_error();
    return message;
}

template <typename Message>
class PublisherLoan {
  public:
    static PublisherLoan borrow(const std::shared_ptr<rclcpp::Publisher<Message>>& publisher) {
        const auto handle = publisher->get_publisher_handle();
        void* message = nullptr;
        const auto& type_support = rclcpp::get_message_type_support_handle<Message>();
        const auto result = rcl_borrow_loaned_message(handle.get(), &type_support, &message);
        if (result != RCL_RET_OK) {
            throw std::runtime_error(error_string("rcl_borrow_loaned_message", result));
        }
        if (message == nullptr) {
            throw std::runtime_error("rcl_borrow_loaned_message returned success with a null message");
        }
        return PublisherLoan(publisher, message);
    }

    PublisherLoan(const PublisherLoan&) = delete;
    PublisherLoan& operator=(const PublisherLoan&) = delete;
    PublisherLoan(PublisherLoan&& other) noexcept
        : publisher_(std::move(other.publisher_))
        , message_(std::exchange(other.message_, nullptr)) {
    }
    PublisherLoan& operator=(PublisherLoan&&) = delete;

    ~PublisherLoan() {
        return_loan_noexcept();
    }

    Message& message() const {
        return *static_cast<Message*>(message_);
    }

    void publish() {
        const auto handle = publisher_->get_publisher_handle();
        const auto result = rcl_publish_loaned_message(handle.get(), message_, nullptr);
        if (result != RCL_RET_OK) {
            throw std::runtime_error(error_string("rcl_publish_loaned_message", result));
        }
        message_ = nullptr;
    }

    void return_loan() {
        const auto handle = publisher_->get_publisher_handle();
        const auto result = rcl_return_loaned_message_from_publisher(handle.get(), message_);
        if (result != RCL_RET_OK) {
            throw std::runtime_error(error_string("rcl_return_loaned_message_from_publisher", result));
        }
        message_ = nullptr;
    }

  private:
    PublisherLoan(std::shared_ptr<rclcpp::Publisher<Message>> publisher, void* message)
        : publisher_(std::move(publisher))
        , message_(message) {
    }

    void return_loan_noexcept() noexcept {
        if (message_ == nullptr || publisher_ == nullptr) {
            return;
        }
        const auto handle = publisher_->get_publisher_handle();
        const auto result = rcl_return_loaned_message_from_publisher(handle.get(), message_);
        if (result != RCL_RET_OK) {
            std::cerr << "ros2_transport_bench: rcl_return_loaned_message_from_publisher failed with RCL code "
                      << result << '\n';
            rcl_reset_error();
        }
        message_ = nullptr;
    }

    std::shared_ptr<rclcpp::Publisher<Message>> publisher_;
    void* message_ { nullptr };
};

class SubscriptionLoan {
  public:
    SubscriptionLoan(const rcl_subscription_t* subscription, void* message)
        : subscription_(subscription)
        , message_(message) {
    }
    SubscriptionLoan(const SubscriptionLoan&) = delete;
    SubscriptionLoan& operator=(const SubscriptionLoan&) = delete;
    ~SubscriptionLoan() {
        return_loan_noexcept();
    }

    void* message() const noexcept {
        return message_;
    }

    void return_loan() {
        const auto result = rcl_return_loaned_message_from_subscription(subscription_, message_);
        if (result != RCL_RET_OK) {
            throw std::runtime_error(error_string("rcl_return_loaned_message_from_subscription", result));
        }
        message_ = nullptr;
    }

  private:
    void return_loan_noexcept() noexcept {
        if (message_ == nullptr || subscription_ == nullptr) {
            return;
        }
        const auto result = rcl_return_loaned_message_from_subscription(subscription_, message_);
        if (result != RCL_RET_OK) {
            std::cerr << "ros2_transport_bench: rcl_return_loaned_message_from_subscription failed with RCL code "
                      << result << '\n';
            rcl_reset_error();
        }
        message_ = nullptr;
    }

    const rcl_subscription_t* subscription_ { nullptr };
    void* message_ { nullptr };
};

std::optional<std::string_view> data_sharing_kind(
    const eprosima::fastdds::dds::DataWriter* writer) {
    if (writer == nullptr) {
        return std::nullopt;
    }
    switch (writer->get_qos().data_sharing().kind()) {
    case eprosima::fastdds::dds::AUTO:
        return "AUTO";
    case eprosima::fastdds::dds::ON:
        return "ON";
    case eprosima::fastdds::dds::OFF:
        return "OFF";
    }
    return std::nullopt;
}

std::optional<std::string_view> data_sharing_kind(
    const eprosima::fastdds::dds::DataReader* reader) {
    if (reader == nullptr) {
        return std::nullopt;
    }
    switch (reader->get_qos().data_sharing().kind()) {
    case eprosima::fastdds::dds::AUTO:
        return "AUTO";
    case eprosima::fastdds::dds::ON:
        return "ON";
    case eprosima::fastdds::dds::OFF:
        return "OFF";
    }
    return std::nullopt;
}

std::optional<std::string_view> participant_transport_kind(
    const eprosima::fastdds::dds::DomainParticipant* participant) {
    if (participant == nullptr) {
        return std::nullopt;
    }
    const auto& transport = participant->get_qos().transport();
    if (transport.use_builtin_transports || transport.user_transports.size() != 1U
        || transport.user_transports.front() == nullptr
        || typeid(*transport.user_transports.front())
            != typeid(eprosima::fastdds::rtps::SharedMemTransportDescriptor)) {
        return std::nullopt;
    }
    return "shm_only";
}

struct PublisherMetadata {
    std::optional<std::string_view> data_sharing;
    std::optional<std::string_view> participant_transport;
};

struct SubscriberMetadata {
    std::optional<std::string_view> data_sharing;
    std::optional<std::string_view> participant_transport;
};

PublisherMetadata inspect_metadata(const rclcpp::PublisherBase& publisher) {
    PublisherMetadata metadata;
    const auto* handle = publisher.get_publisher_handle().get();
    const auto* rmw_handle = handle == nullptr ? nullptr : rcl_publisher_get_rmw_handle(handle);
    if (rmw_handle == nullptr || rmw_handle->implementation_identifier == nullptr
        || std::strcmp(rmw_handle->implementation_identifier, "rmw_fastrtps_cpp") != 0
        || rmw_handle->data == nullptr) {
        return metadata;
    }
    const auto* info = static_cast<const CustomPublisherInfo*>(rmw_handle->data);
    metadata.data_sharing = data_sharing_kind(info->data_writer_);
    const auto* dds_publisher = info->data_writer_ == nullptr
        ? nullptr
        : info->data_writer_->get_publisher();
    metadata.participant_transport = participant_transport_kind(
        dds_publisher == nullptr ? nullptr : dds_publisher->get_participant());
    return metadata;
}

SubscriberMetadata inspect_metadata(const rclcpp::SubscriptionBase& subscription) {
    SubscriberMetadata metadata;
    const auto* handle = subscription.get_subscription_handle().get();
    const auto* rmw_handle = handle == nullptr ? nullptr : rcl_subscription_get_rmw_handle(handle);
    if (rmw_handle == nullptr || rmw_handle->implementation_identifier == nullptr
        || std::strcmp(rmw_handle->implementation_identifier, "rmw_fastrtps_cpp") != 0
        || rmw_handle->data == nullptr) {
        return metadata;
    }
    const auto* info = static_cast<const CustomSubscriberInfo*>(rmw_handle->data);
    metadata.data_sharing = data_sharing_kind(info->data_reader_);
    const auto* dds_subscriber = info->data_reader_ == nullptr
        ? nullptr
        : info->data_reader_->get_subscriber();
    metadata.participant_transport = participant_transport_kind(
        dds_subscriber == nullptr ? nullptr : dds_subscriber->get_participant());
    return metadata;
}

template <typename Message>
void run_publisher(const Options& options, Window& window, Metrics& metrics,
                   const std::shared_ptr<rclcpp::Node>& node) {
    auto publisher = node->create_publisher<Message>(
        options.topic,
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile());
    const bool can_loan = publisher->can_loan_messages();
    const bool use_loan = options.mode == Mode::Loan;
    const auto metadata = inspect_metadata(*publisher);
    if (use_loan && !can_loan) {
        write_ready(options, Backend::Ros2, false, false,
                    metadata.data_sharing, metadata.participant_transport,
                    true, "publisher middleware does not support loaned messages");
        throw std::runtime_error("ROS 2 publisher middleware does not support loaned messages");
    }
    write_ready(options, Backend::Ros2, can_loan, use_loan,
                metadata.data_sharing, metadata.participant_transport);
    window = wait_for_window_file(options.window_file);

    if (use_loan) {
        run_publish_schedule(options, window, metrics, true,
            [&publisher](std::uint64_t sequence,
                         std::int64_t end_ns,
                         std::size_t expected_bytes,
                         std::uintptr_t& address) -> std::optional<std::int64_t> {
                auto loan = PublisherLoan<Message>::borrow(publisher);
                auto& message = loan.message();
                fill_message(message, sequence, expected_bytes);
                address = reinterpret_cast<std::uintptr_t>(std::addressof(message));
                const auto sent_ns = static_cast<std::int64_t>(message.sent_ns);
                if (sent_ns >= end_ns || monotonic_now_ns() >= end_ns) {
                    loan.return_loan();
                    return std::nullopt;
                }
                loan.publish();
                return sent_ns;
            });
        return;
    }

    // The reusable heap message keeps multi-megabyte fixed arrays off the stack.
    auto message = std::make_unique<Message>();
    run_publish_schedule(options, window, metrics, false,
        [&publisher, &message](std::uint64_t sequence,
                               std::int64_t end_ns,
                               std::size_t expected_bytes,
                               std::uintptr_t& address) -> std::optional<std::int64_t> {
            fill_message(*message, sequence, expected_bytes);
            address = reinterpret_cast<std::uintptr_t>(message.get());
            const auto sent_ns = static_cast<std::int64_t>(message->sent_ns);
            if (sent_ns >= end_ns || monotonic_now_ns() >= end_ns) {
                return std::nullopt;
            }
            publisher->publish(*message);
            return sent_ns;
        });
}

template <typename Message>
bool take_loaned_sample(const std::shared_ptr<rclcpp::Subscription<Message>>& subscription,
                        Metrics& metrics,
                        const Window& window,
                        std::size_t expected_bytes) {
    const auto handle = subscription->get_subscription_handle();
    void* raw_message = nullptr;
    auto message_info = rmw_get_zero_initialized_message_info();
    const auto result = rcl_take_loaned_message(handle.get(), &raw_message, &message_info, nullptr);
    if (result == RCL_RET_SUBSCRIPTION_TAKE_FAILED) {
        return false;
    }
    if (result != RCL_RET_OK) {
        throw std::runtime_error(error_string("rcl_take_loaned_message", result));
    }
    if (raw_message == nullptr) {
        throw std::runtime_error("rcl_take_loaned_message returned success with a null message");
    }
    SubscriptionLoan loan(handle.get(), raw_message);
    auto* message = static_cast<Message*>(loan.message());
    const auto receive_ns = monotonic_now_ns();
    const auto address = reinterpret_cast<std::uintptr_t>(message);
    record_received(metrics, *message, receive_ns, window, expected_bytes, address, true);
    loan.return_loan();
    return true;
}

template <typename Message>
void run_subscriber(const Options& options, Window& window, Metrics& metrics,
                    const std::shared_ptr<rclcpp::Node>& node) {
    const auto spec = case_spec(options.bench_case);
    auto subscription = node->create_subscription<Message>(
        options.topic,
        rclcpp::QoS(rclcpp::KeepLast(1)).best_effort().durability_volatile(),
        [](const Message&) {});
    const bool can_loan = subscription->can_loan_messages();
    const bool use_loan = options.mode == Mode::Loan;
    const auto metadata = inspect_metadata(*subscription);
    if (use_loan && !can_loan) {
        write_ready(options, Backend::Ros2, false, false,
                    metadata.data_sharing, metadata.participant_transport,
                    true, "subscriber middleware does not support loaned messages");
        throw std::runtime_error("ROS 2 subscriber middleware does not support loaned messages");
    }

    rclcpp::WaitSet wait_set;
    wait_set.add_subscription(subscription);
    std::unique_ptr<Message> copy_message;
    if (!use_loan) {
        copy_message = std::make_unique<Message>();
    }
    write_ready(options, Backend::Ros2, can_loan, use_loan,
                metadata.data_sharing, metadata.participant_transport);
    window = wait_for_window_file(options.window_file);
    const auto stop_at = window.end_ns + kShutdownDrainGraceNs;

    while (monotonic_now_ns() < stop_at) {
        const auto now = monotonic_now_ns();
        const auto wait_ns = std::max<std::int64_t>(stop_at - now, 0LL);
        auto wait_result = wait_set.wait(std::chrono::nanoseconds(wait_ns));
        if (wait_result.kind() != rclcpp::WaitResultKind::Ready) {
            continue;
        }
        // This wait set owns exactly one subscription. Take directly after a
        // ready wakeup; an empty/spurious wakeup is handled by take returning false.
        // This also supports Humble, which lacks next_ready_subscription().

        for (std::size_t drained = 0U; drained < 64U; ++drained) {
            if (use_loan) {
                if (!take_loaned_sample(subscription, metrics, window, spec.payload_bytes)) {
                    break;
                }
                if (monotonic_now_ns() >= stop_at) {
                    break;
                }
                continue;
            }
            rclcpp::MessageInfo message_info;
            if (!subscription->take(*copy_message, message_info)) {
                break;
            }
            const auto receive_ns = monotonic_now_ns();
            const auto address = reinterpret_cast<std::uintptr_t>(copy_message.get());
            record_received(metrics,
                            *copy_message,
                            receive_ns,
                            window,
                            spec.payload_bytes,
                            address,
                            false);
            if (monotonic_now_ns() >= stop_at) {
                break;
            }
        }
    }
}

template <typename Message>
void run_case(const Options& options, Window& window, Metrics& metrics,
              const std::shared_ptr<rclcpp::Node>& node) {
    if (options.role == Role::Publisher) {
        run_publisher<Message>(options, window, metrics, node);
    } else {
        run_subscriber<Message>(options, window, metrics, node);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::optional<transport_bench::Options> options;
    transport_bench::Window window;
    transport_bench::Metrics metrics;
    metrics.address_probes.reserve(8U);
    bool initialized = false;
    try {
        options = transport_bench::parse_options(argc, argv, transport_bench::Backend::Ros2);
        rclcpp::init(0, nullptr);
        initialized = true;
        rclcpp::NodeOptions node_options;
        node_options.use_intra_process_comms(false);
        const auto role = transport_bench::name(options->role);
        const auto node_name = "transport_bench_" + std::string(role) + "_" + std::to_string(::getpid());
        auto node = std::make_shared<rclcpp::Node>(node_name, node_options);
        switch (options->bench_case) {
        case transport_bench::BenchCase::Imu:
            run_case<ImuFrame>(*options, window, metrics, node);
            break;
        case transport_bench::BenchCase::Cloud:
            run_case<CloudFrame>(*options, window, metrics, node);
            break;
        case transport_bench::BenchCase::Video:
            run_case<VideoFrame>(*options, window, metrics, node);
            break;
        }
        transport_bench::write_output(*options, transport_bench::Backend::Ros2, window, metrics);
        rclcpp::shutdown();
        return metrics.invalid == 0U ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "ros2_transport_bench: " << error.what() << '\n';
        if (options.has_value()) {
            try {
                transport_bench::write_output(
                    options.value(), transport_bench::Backend::Ros2, window, metrics, error.what());
            } catch (const std::exception& output_error) {
                std::cerr << "ros2_transport_bench: could not write output JSON: "
                          << output_error.what() << '\n';
            }
        }
        if (initialized) {
            rclcpp::shutdown();
        }
        return 1;
    }
}

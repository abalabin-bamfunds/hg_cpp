#pragma once

#include <bg/graph/output_notification.hpp>
#include <bg/time_series/ref/ref_storage.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bg {

class TimeSeriesRuntimeType;

namespace details {

class ReferenceInputSubscriber {
  public:
    ReferenceInputSubscriber(std::uint32_t input_offset, std::uint32_t node_index, bool active, bool required,
                             bool controlled_activity, const TimeSeriesRuntimeType &requested_runtime_type) noexcept
        : requested_runtime_type_{&requested_runtime_type}, input_offset_{input_offset},
          node_index_and_flags_{node_index | (active ? active_flag : 0) | (required ? required_flag : 0) |
                                (controlled_activity ? controlled_activity_flag : 0)} {}

    const TimeSeriesRuntimeType &requested_runtime_type() const noexcept { return *requested_runtime_type_; }
    std::size_t input_offset() const noexcept { return input_offset_; }
    std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }
    bool active() const noexcept { return (node_index_and_flags_ & active_flag) != 0; }
    bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }
    bool controlled_activity() const noexcept { return (node_index_and_flags_ & controlled_activity_flag) != 0; }

    void update_input(OutputChangeContext &context, Reference reference, std::size_t notification_data,
                      InputNotificationFunction notification, OutputChange change) const;

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t controlled_activity_flag = std::uint32_t{1} << 29;
    static constexpr std::uint32_t node_index_mask = controlled_activity_flag - 1;

    const TimeSeriesRuntimeType *requested_runtime_type_;
    std::uint32_t input_offset_;
    std::uint32_t node_index_and_flags_;
};

class ReferenceInputNotificationSubscriber {
  public:
    ReferenceInputNotificationSubscriber(std::uint32_t input_offset, std::size_t notification_data,
                                         std::uint32_t node_index, bool active, bool required, bool controlled_activity,
                                         const TimeSeriesRuntimeType &requested_runtime_type,
                                         InputNotificationFunction notification) noexcept
        : subscription_{input_offset, node_index, active, required, controlled_activity, requested_runtime_type},
          notification_data_{notification_data}, notification_{notification} {}

    const ReferenceInputSubscriber &subscription() const noexcept { return subscription_; }
    std::size_t notification_data() const noexcept { return notification_data_; }
    InputNotificationFunction notification() const noexcept { return notification_; }

  private:
    ReferenceInputSubscriber subscription_;
    std::size_t notification_data_;
    InputNotificationFunction notification_;
};

void notify_reference_inputs(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type, const void *value,
                             OutputChange change, std::span<const std::byte> records);
void notify_reference_inputs_with_notifications(OutputChangeContext &context, const TimeSeriesRuntimeType *runtime_type,
                                                const void *value, OutputChange change,
                                                std::span<const std::byte> records);

static_assert(sizeof(ReferenceInputSubscriber) == 2 * sizeof(void *));
static_assert(sizeof(ReferenceInputNotificationSubscriber) == 4 * sizeof(void *));

} // namespace details
} // namespace bg

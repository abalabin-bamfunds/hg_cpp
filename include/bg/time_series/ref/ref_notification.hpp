#pragma once

#include <bg/graph/output_notification.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bg {

class TimeSeriesType;

namespace details {

class ReferenceInputSubscriber {
  public:
    ReferenceInputSubscriber(std::uint32_t input_offset, std::uint32_t node_index, bool active, bool required,
                             const TimeSeriesType &requested_type) noexcept
        : requested_type_{&requested_type}, input_offset_{input_offset},
          node_index_and_flags_{node_index | (active ? active_flag : 0) | (required ? required_flag : 0)} {}

    [[nodiscard]] const TimeSeriesType &requested_type() const noexcept { return *requested_type_; }
    [[nodiscard]] std::size_t input_offset() const noexcept { return input_offset_; }
    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }
    [[nodiscard]] bool active() const noexcept { return (node_index_and_flags_ & active_flag) != 0; }
    [[nodiscard]] bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t node_index_mask = required_flag - 1;

    const TimeSeriesType *requested_type_;
    std::uint32_t input_offset_;
    std::uint32_t node_index_and_flags_;
};

class CallbackReferenceInputSubscriber {
  public:
    CallbackReferenceInputSubscriber(std::uint32_t input_offset, std::size_t data, std::uint32_t node_index,
                                     bool active, bool required, const TimeSeriesType &requested_type,
                                     InputNotificationFunction callback) noexcept
        : subscription_{input_offset, node_index, active, required, requested_type}, data_{data}, callback_{callback} {}

    [[nodiscard]] const ReferenceInputSubscriber &subscription() const noexcept { return subscription_; }
    [[nodiscard]] std::size_t data() const noexcept { return data_; }
    [[nodiscard]] InputNotificationFunction callback() const noexcept { return callback_; }

  private:
    ReferenceInputSubscriber subscription_;
    std::size_t data_;
    InputNotificationFunction callback_;
};

void notify_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                             std::span<const std::byte> records);
void notify_callback_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                                      std::span<const std::byte> records);

static_assert(sizeof(ReferenceInputSubscriber) == 2 * sizeof(void *));
static_assert(sizeof(CallbackReferenceInputSubscriber) == 4 * sizeof(void *));

} // namespace details
} // namespace bg

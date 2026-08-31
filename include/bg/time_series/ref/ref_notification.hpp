#pragma once

#include <bg/graph/output_notification.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace bg::details {

class ReferenceInputSubscriber {
  public:
    ReferenceInputSubscriber(std::uint32_t input_offset, std::uint32_t node_index, bool active, bool required) noexcept
        : input_offset_{input_offset},
          node_index_and_flags_{node_index | (active ? active_flag : 0) | (required ? required_flag : 0)} {}

    [[nodiscard]] std::size_t input_offset() const noexcept { return input_offset_; }
    [[nodiscard]] std::size_t node_index() const noexcept { return node_index_and_flags_ & node_index_mask; }
    [[nodiscard]] bool active() const noexcept { return (node_index_and_flags_ & active_flag) != 0; }
    [[nodiscard]] bool required() const noexcept { return (node_index_and_flags_ & required_flag) != 0; }

  private:
    static constexpr std::uint32_t active_flag = std::uint32_t{1} << 31;
    static constexpr std::uint32_t required_flag = std::uint32_t{1} << 30;
    static constexpr std::uint32_t node_index_mask = required_flag - 1;

    std::uint32_t input_offset_;
    std::uint32_t node_index_and_flags_;
};

void notify_reference_inputs(OutputNotificationContext &context, const void *value, OutputChange change,
                             std::span<const std::byte> records);

static_assert(sizeof(ReferenceInputSubscriber) == 2 * sizeof(std::uint32_t));

} // namespace bg::details

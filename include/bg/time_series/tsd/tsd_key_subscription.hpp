#pragma once

#include <bg/scalar/scalar.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

[[nodiscard]] Reference acquire_tsd_item_reference(Reference tsd, Scalar<> key);
void release_tsd_item_reference(Reference item) noexcept;

void add_tsd_key_subscriber(Reference tsd, Scalar<> key, RuntimeGraph &graph, std::uint32_t node_index);
void remove_tsd_key_subscriber(Reference tsd, RuntimeGraph &graph, std::uint32_t node_index) noexcept;

} // namespace bg::details

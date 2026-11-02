#pragma once

#include <hg/node/input_property_types.hpp>
#include <hg/node/details/input_properties.hpp>

namespace hg {

template <auto... Properties> using InputProperties = details::input_properties<Properties...>;

} // namespace hg

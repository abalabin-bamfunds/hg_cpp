#pragma once

#include <bg/node/input_property_types.hpp>
#include <bg/node/details/input_properties.hpp>

namespace bg {

template <auto... Properties> using InputProperties = details::input_properties<Properties...>;

} // namespace bg

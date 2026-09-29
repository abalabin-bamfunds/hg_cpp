#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <nanobind/nanobind.h>

namespace bg {
class GraphWiring;
class Type;
} // namespace bg

namespace bg::python::details {

WiringArgument wiring_argument(const Type &expected, nanobind::handle value, allocator &memory, GraphWiring &wiring);
WiringArgument wiring_argument(nanobind::handle value, allocator &memory, GraphWiring &wiring);

} // namespace bg::python::details

#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <vector>

namespace bg {
class GraphWiring;
class Type;
class Signature;
} // namespace bg

namespace bg::python::details {

WiringArgument wiring_argument(const Type &expected, nanobind::handle value, allocator &memory, GraphWiring &wiring);
WiringArgument wiring_argument(nanobind::handle value, allocator &memory, GraphWiring &wiring);
std::vector<WiringArgument> wiring_arguments(const Signature &signature, nanobind::sequence fixed_values,
                                             nanobind::sequence positional_values, nanobind::dict named_values,
                                             allocator &memory, GraphWiring &wiring,
                                             std::span<const Type *const> parameter_types = {});

} // namespace bg::python::details

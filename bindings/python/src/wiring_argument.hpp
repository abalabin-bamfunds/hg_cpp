#pragma once

#include <hg/allocator.hpp>
#include <hg/graph/wiring_argument.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <vector>

namespace hg {
class GraphWiring;
class Type;
class Signature;
} // namespace hg

namespace hg::python::details {

WiringArgument wiring_argument(const Type &expected, nanobind::handle value, allocator &memory, GraphWiring &wiring);
WiringArgument wiring_argument(nanobind::handle value, allocator &memory, GraphWiring &wiring);
std::vector<WiringArgument> wiring_arguments(const Signature &signature, nanobind::sequence fixed_values,
                                             nanobind::sequence positional_values, nanobind::dict named_values,
                                             allocator &memory, GraphWiring &wiring,
                                             std::span<const Type *const> parameter_types = {});

} // namespace hg::python::details

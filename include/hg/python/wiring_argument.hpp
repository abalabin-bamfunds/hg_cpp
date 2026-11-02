#pragma once

#include <hg/allocator.hpp>
#include <hg/graph/wiring_argument.hpp>
#include <hg/python/export.hpp>

#include <nanobind/nanobind.h>

#include <span>
#include <vector>

namespace hg {
class GraphWiring;
class Signature;
class Type;
} // namespace hg

namespace hg::python::details {

HG_PYTHON_API std::vector<WiringArgument>
wiring_arguments(const Signature &signature, nanobind::sequence fixed_values,
                 nanobind::sequence positional_values, nanobind::dict named_values, allocator &memory,
                 GraphWiring &wiring, std::span<const Type *const> parameter_types = {});

} // namespace hg::python::details

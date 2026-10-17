#pragma once

#include <bg/allocator.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/queue_mode.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_type.hpp>

#include <nanobind/nanobind.h>

#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace bg::python::details {

// Owns scalar and nested literal backing until serialization completes; literals borrow this storage.
class PythonLiteralBuilder {
  public:
    PythonLiteralBuilder();

    literal::TimeSeriesLiteral delta(const TimeSeriesType &type, nanobind::handle value,
                                     QueueMode mode = QueueMode::standard);
    std::span<const literal::TimeSeriesLiteral> sequence(std::vector<literal::TimeSeriesLiteral> values);

  private:
    literal::TimeSeriesLiteral scalar(const ScalarType &type, nanobind::handle value);
    literal::ScalarLiteral &removed_value(const ScalarType &type, nanobind::handle value);

    std::pmr::unsynchronized_pool_resource pool_;
    allocator memory_;
    std::vector<std::unique_ptr<ScalarValue<>>> scalars_;
    std::vector<std::unique_ptr<literal::ScalarLiteral>> removed_values_;
    std::vector<std::unique_ptr<literal::TaggedLiteral>> removed_markers_;
    std::vector<std::unique_ptr<std::vector<literal::TimeSeriesLiteral>>> sequences_;
};

} // namespace bg::python::details

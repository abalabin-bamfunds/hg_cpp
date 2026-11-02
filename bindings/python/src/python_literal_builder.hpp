#pragma once

#include <hg/allocator.hpp>
#include <hg/literal/sentinel.hpp>
#include <hg/literal/time_series_literal.hpp>
#include <hg/queue_mode.hpp>
#include <hg/scalar/value.hpp>
#include <hg/type/scalar_type.hpp>
#include <hg/type/time_series_type.hpp>

#include <nanobind/nanobind.h>

#include <memory>
#include <memory_resource>
#include <span>
#include <vector>

namespace hg::python::details {

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

} // namespace hg::python::details

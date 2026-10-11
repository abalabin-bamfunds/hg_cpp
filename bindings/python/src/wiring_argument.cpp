#include "wiring_argument.hpp"

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/operators.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/type/type.hpp>

#include <array>
#include <stdexcept>
#include <string>

namespace nb = nanobind;

namespace bg::python::details {

WiringArgument wiring_argument(const Type &expected, nb::handle value, allocator &memory, GraphWiring &wiring) {
    if (nb::isinstance<PythonWiringPoint>(value)) {
        return nb::cast<PythonWiringPoint>(value).argument(wiring);
    }

    const auto *scalar_type = type_cast<ScalarType>(&expected);
    const auto *time_series_type = type_cast<TimeSeriesValueType>(&expected);
    if (scalar_type == nullptr && time_series_type == nullptr) {
        throw std::invalid_argument{"Python node and graph arguments require scalar or TS[T] types"};
    }

    const auto expects_scalar = scalar_type != nullptr;
    if (time_series_type != nullptr) {
        scalar_type = &time_series_type->value_type();
    }

    if (value.is_none() && expects_scalar) {
        return WiringArgument::absent_scalar(*scalar_type, resolve_scalar_runtime_type(*scalar_type, {}));
    }
    if (value.is_none()) {
        throw std::invalid_argument{"None cannot be wired to a time-series input"};
    }

    auto scalar = [&] {
        try {
            return scalar_conversions().from_python(*scalar_type, value, memory);
        } catch (nb::python_error &) {
            throw;
        } catch (const std::exception &error) {
            auto message = std::string{"cannot convert Python value to scalar type "};
            scalar_type->format_name(message);
            message += ": ";
            message += error.what();
            throw std::invalid_argument{std::move(message)};
        }
    }();
    if (expects_scalar) {
        return WiringArgument{std::move(scalar)};
    }

    auto arguments = std::array{WiringArgument{std::move(scalar)}};
    const auto point =
        static_cast<const WiringTemplate &>(operators::const_)(std::span<const WiringArgument>{arguments});
    if (!point) {
        throw std::logic_error{"the const operator did not produce a time series"};
    }
    return WiringArgument{*point};
}

WiringArgument wiring_argument(nb::handle value, allocator &memory, GraphWiring &wiring) {
    if (nb::isinstance<PythonWiringPoint>(value)) {
        return nb::cast<PythonWiringPoint>(value).argument(wiring);
    }
    if (value.is_none()) {
        throw std::invalid_argument{"None does not identify a scalar type for operator resolution"};
    }

    return WiringArgument{scalar_conversions().from_python(value, memory)};
}

} // namespace bg::python::details

namespace bg::python {

WiringArgument PythonWiringPoint::argument(const GraphWiring &wiring) const {
    const auto point = get(wiring);
    if (!point.has_output()) {
        throw std::invalid_argument{"a Python wiring point must have an output"};
    }

    auto result = WiringArgument{point};
    if (requires_optional_) {
        result.require_optional();
    }
    return result;
}

PythonWiringPoint PythonWiringPoint::as_optional() const {
    lifetime_->check();
    if (!point_.has_output()) {
        throw std::invalid_argument{"opt requires a Python wiring point with an output"};
    }

    return {point_, lifetime_, true};
}

} // namespace bg::python

#include "wiring_argument.hpp"

#include <hg/graph/details/variadic_wiring_record.hpp>
#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/wiring_template.hpp>
#include <hg/operators.hpp>
#include <hg/python/scalar_conversion.hpp>
#include <hg/python/wiring_point.hpp>
#include <hg/type/signature.hpp>
#include <hg/type/type.hpp>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;

namespace hg::python::details {

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
        return WiringArgument::absent_scalar(*scalar_type, resolve_default_scalar_runtime_type(*scalar_type));
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

std::vector<WiringArgument> wiring_arguments(const Signature &signature, nb::sequence fixed_values,
                                             nb::sequence positional_values, nb::dict named_values, allocator &memory,
                                             GraphWiring &wiring, std::span<const Type *const> parameter_types) {
    const auto fixed_parameter_count = signature.first_variadic_parameter().value_or(signature.parameter_count());
    auto fixed_parameters = std::vector<std::size_t>{};
    fixed_parameters.reserve(fixed_parameter_count);
    for (auto index = std::size_t{}; index < fixed_parameter_count; ++index) {
        if (!signature.parameter(index).context) {
            fixed_parameters.push_back(index);
        }
    }
    if (nb::len(fixed_values) != fixed_parameters.size()) {
        throw std::invalid_argument{"the Python definition received the wrong number of fixed arguments"};
    }

    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(fixed_parameters.size() + nb::len(positional_values) + nb::len(named_values));
    for (auto index = std::size_t{}; index < fixed_parameters.size(); ++index) {
        const auto parameter_index = fixed_parameters[index];
        const auto &expected = parameter_types.empty() ? *signature.parameter(parameter_index).type
                                                       : *parameter_types[parameter_index];
        arguments.push_back(wiring_argument(expected, fixed_values[index], memory, wiring));
    }

    for (auto index = std::size_t{}; index < nb::len(positional_values); ++index) {
        auto argument = wiring_argument(positional_values[index], memory, wiring);
        argument.set_variadic_name(util::symbol{std::to_string(index)}, hg::details::VariadicMode::positional);
        arguments.push_back(std::move(argument));
    }

    const auto positional_parameter = signature.variadic_parameter(hg::details::VariadicMode::positional);
    const auto named_parameter = signature.variadic_parameter(hg::details::VariadicMode::named);
    for (const auto [name, value] : named_values) {
        auto argument = wiring_argument(value, memory, wiring);
        const auto argument_name = util::symbol{nb::cast<std::string>(name)};
        const auto fixed_parameter = [&] {
            for (auto index = std::size_t{}; index < fixed_parameter_count; ++index) {
                if (signature.parameter(index).name == argument_name) {
                    return true;
                }
            }
            return false;
        }();
        if (fixed_parameter) {
            argument.set_variadic_name(argument_name, hg::details::VariadicMode::none);
            argument.match_fixed_parameter_by_name();
            arguments.push_back(std::move(argument));
            continue;
        }
        const auto exact_parameter =
            (positional_parameter && signature.parameter(*positional_parameter).name == argument_name) ||
            (named_parameter && signature.parameter(*named_parameter).name == argument_name);
        if (exact_parameter && argument.is_time_series() &&
            hg::details::variadic_wiring_record(argument.time_series()) != nullptr) {
            argument.clear_name();
        } else {
            argument.set_variadic_name(argument_name, exact_parameter ? hg::details::VariadicMode::none
                                                                      : hg::details::VariadicMode::named);
        }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

} // namespace hg::python::details

namespace hg::python {

WiringArgument PythonWiringPoint::argument(const GraphWiring &wiring) const {
    const auto point = get(wiring);
    if (!point.has_output()) {
        throw std::invalid_argument{"a Python wiring point must have an output"};
    }

    auto result = WiringArgument{point};
    if (requires_optional_) {
        result.require_optional();
    }
    if (requires_valid_items_) {
        result.require_valid_items();
    }
    return result;
}

PythonWiringPoint PythonWiringPoint::as_optional() const {
    lifetime_->check();
    if (!point_.has_output()) {
        throw std::invalid_argument{"opt requires a Python wiring point with an output"};
    }

    return {point_, lifetime_, true, requires_valid_items_};
}

PythonWiringPoint PythonWiringPoint::as_valid_items() const {
    lifetime_->check();
    if (!point_.has_output()) {
        throw std::invalid_argument{"valid_items requires a Python wiring point with an output"};
    }

    return {point_, lifetime_, requires_optional_, true};
}

} // namespace hg::python

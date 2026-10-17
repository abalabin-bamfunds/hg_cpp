#include "test_graph_input.hpp"

#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <stdexcept>
#include <utility>

namespace nb = nanobind;

namespace bg::python::details {

TestGraphInputs::TestGraphInputs(const Signature &signature, std::span<const Type *const> wiring_parameter_types,
                                 nb::sequence input_sequences) {
    if (nb::len(input_sequences) != signature.parameter_count()) {
        throw std::invalid_argument{"test_graph requires one update sequence per graph parameter"};
    }
    if (wiring_parameter_types.size() != signature.parameter_count()) {
        throw std::invalid_argument{"test_graph wiring types do not match the graph signature"};
    }

    inputs_.reserve(signature.parameter_count());
    for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
        const auto &time_series_type = input_type(*signature.parameter(index).type, *wiring_parameter_types[index]);
        auto ticks = nb::cast<nb::sequence>(input_sequences[index]);
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        values.reserve(nb::len(ticks));
        for (const auto tick : ticks) {
            values.push_back(literals_.delta(time_series_type, tick));
        }

        const auto &runtime_type = resolve_time_series_runtime_type(time_series_type);
        inputs_.emplace_back(runtime_type, literals_.sequence(std::move(values)));
    }
}

const TimeSeriesType &TestGraphInputs::input_type(const Type &parameter_type, const Type &wiring_type) const {
    const auto *time_series = type_cast<TimeSeriesType>(&parameter_type);
    if (time_series == nullptr) {
        time_series = type_cast<TimeSeriesType>(&wiring_type);
    }

    if (time_series != nullptr) {
        if (time_series->is_resolved()) {
            return *time_series;
        }

        auto type_map = TypeMap{};
        if (const auto *guarantee = time_series->guarantee().variable()) {
            type_map.set(*guarantee, guarantee->minimum().value_or(TimeSeriesGuarantee::ordinary));
        }

        try {
            const auto *resolved = type_cast<TimeSeriesType>(&time_series->resolve(type_map));
            if (resolved != nullptr && resolved->is_resolved()) {
                return *resolved;
            }
        } catch (const TypeResolutionError &error) {
            if (error.code() != TypeResolutionErrorCode::unmapped_variable) {
                throw;
            }
        }

        throw std::invalid_argument{
            "test_graph needs fully resolved input types; specialize the graph with test_graph(g[...], ...)"};
    }

    if (const auto *scalar_type = type_cast<ScalarType>(&wiring_type)) {
        try {
            const auto *resolved = type_cast<ScalarType>(&scalar_type->resolve(TypeMap{}));
            if (resolved != nullptr && resolved->is_resolved()) {
                return time_series_value_type(*resolved, TimeSeriesGuarantee::ordinary);
            }
        } catch (const TypeResolutionError &error) {
            if (error.code() != TypeResolutionErrorCode::unmapped_variable) {
                throw;
            }
        }
    }

    throw std::invalid_argument{"test_graph inputs must resolve to time-series types"};
}

} // namespace bg::python::details

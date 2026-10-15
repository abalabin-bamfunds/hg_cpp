#include "test_graph_input.hpp"

#include <bg/python/scalar_conversion.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include "delta.hpp"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

#include <Python.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

nb::object mapping_items(nb::handle value) {
    if (PyMapping_Check(value.ptr()) == 0 || PySequence_Check(value.ptr()) != 0) {
        PyErr_SetString(PyExc_TypeError, "expected a mapping for a time-series collection delta");
        nb::raise_python_error();
    }

    auto items = nb::steal<nb::object>(PyMapping_Items(value.ptr()));
    if (!items) {
        nb::raise_python_error();
    }
    return items;
}

template <typename Function> void for_each_mapping_item(nb::handle value, Function &&function) {
    auto items = mapping_items(value);
    for (const auto item : nb::borrow<nb::iterable>(items)) {
        const auto pair = nb::cast<nb::tuple>(item);
        if (nb::len(pair) != 2) {
            throw std::invalid_argument{"a mapping returned an invalid item"};
        }
        function(pair[0], pair[1]);
    }
}

template <typename Function> void for_each_set_item(nb::handle value, Function &&function) {
    if (!PySet_Check(value.ptr()) && !PyFrozenSet_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "TSS deltas require a set or frozenset");
        nb::raise_python_error();
    }

    for (const auto item : nb::borrow<nb::iterable>(value)) {
        function(item);
    }
}

} // namespace

TestGraphInputs::TestGraphInputs(const Signature &signature, std::span<const Type *const> wiring_parameter_types,
                                 nb::sequence input_sequences)
    : memory_{pool_} {
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
            values.push_back(delta(time_series_type, tick));
        }

        const auto &runtime_type = resolve_time_series_runtime_type(time_series_type);
        sequences_.push_back(std::make_unique<std::vector<literal::TimeSeriesLiteral>>(std::move(values)));
        inputs_.emplace_back(runtime_type, std::span<const literal::TimeSeriesLiteral>{*sequences_.back()});
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

literal::TimeSeriesLiteral TestGraphInputs::scalar(const ScalarType &type, nb::handle value) {
    auto stored = std::make_unique<ScalarValue<>>(scalar_conversions().from_python(type, value, memory_));
    const auto &result = *stored;
    scalars_.push_back(std::move(stored));
    return literal::TimeSeriesLiteral(result);
}

literal::ScalarLiteral &TestGraphInputs::removed_value(const ScalarType &type, nb::handle value) {
    auto stored = std::make_unique<ScalarValue<>>(scalar_conversions().from_python(type, value, memory_));
    auto literal_value = std::make_unique<literal::ScalarLiteral>(*stored);
    auto &result = *literal_value;
    scalars_.push_back(std::move(stored));
    removed_values_.push_back(std::move(literal_value));
    return result;
}

literal::TimeSeriesLiteral TestGraphInputs::sequence(std::vector<literal::TimeSeriesLiteral> values) {
    auto stored = std::make_unique<std::vector<literal::TimeSeriesLiteral>>(std::move(values));
    const auto view = std::span<const literal::TimeSeriesLiteral>{*stored};
    sequences_.push_back(std::move(stored));
    return literal::TimeSeriesLiteral(view);
}

literal::TimeSeriesLiteral TestGraphInputs::delta(const TimeSeriesType &type, nb::handle value) {
    if (value.is_none()) {
        return literal::TimeSeriesLiteral(literal::TimeSeriesLiteralItem{literal::SKIP});
    }
    if (is_invalid(value)) {
        if (!type.supports_invalidation()) {
            throw std::invalid_argument{"this time-series input type does not support invalidation"};
        }
        return literal::TimeSeriesLiteral(literal::TimeSeriesLiteralItem{literal::INVALID});
    }
    if (const auto *time_series = type_cast<TimeSeriesValueType>(&type)) {
        return scalar(time_series->value_type(), value);
    }

    if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        auto append = [&](nb::handle item, bool remove) {
            if (is_removed(item)) {
                item = nb::getattr(item, "item");
                remove = true;
            }
            if (remove) {
                values.emplace_back(literal::TimeSeriesLiteralItem{
                    literal::REMOVED(removed_value(set->item_type(), item))});
            } else {
                values.push_back(scalar(set->item_type(), item));
            }
        };

        if (is_set_delta(value)) {
            const auto additions = nb::getattr(value, "added");
            const auto removals = nb::getattr(value, "removed");
            for_each_set_item(additions, [&](nb::handle item) { append(item, false); });
            for_each_set_item(removals, [&](nb::handle item) { append(item, true); });
        } else {
            for_each_set_item(value, [&](nb::handle item) { append(item, false); });
        }
        return sequence(std::move(values));
    }

    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        for_each_mapping_item(value, [&](nb::handle key, nb::handle item) {
            if (PyBool_Check(key.ptr())) {
                PyErr_SetString(PyExc_TypeError, "a TSL index must be an integer, not bool");
                nb::raise_python_error();
            }
            const auto index = nb::cast<std::int64_t>(key);
            if (index < 0 || static_cast<std::uint64_t>(index) >= list->item_count()) {
                throw std::out_of_range{"a TSL update index is out of range"};
            }
            auto pair = std::vector<literal::TimeSeriesLiteral>{};
            pair.reserve(2);
            pair.push_back(scalar(int64_type(), nb::int_(index)));
            pair.push_back(delta(list->item_type(static_cast<std::size_t>(index)), item));
            values.push_back(sequence(std::move(pair)));
        });
        return sequence(std::move(values));
    }

    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type)) {
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        for_each_mapping_item(value, [&](nb::handle key, nb::handle item) {
            if (!PyUnicode_Check(key.ptr())) {
                PyErr_SetString(PyExc_TypeError, "a TSB field name must be a string");
                nb::raise_python_error();
            }
            const auto name = nb::cast<std::string>(key);
            const auto index = bundle->find_item_index(util::symbol{name});
            if (!index) {
                throw std::invalid_argument{"a TSB update contains an unknown field"};
            }
            auto pair = std::vector<literal::TimeSeriesLiteral>{};
            pair.reserve(2);
            pair.push_back(scalar(symbol_type(), key));
            pair.push_back(delta(bundle->item_type(*index), item));
            values.push_back(sequence(std::move(pair)));
        });
        return sequence(std::move(values));
    }

    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        for_each_mapping_item(value, [&](nb::handle key, nb::handle item) {
            auto pair = std::vector<literal::TimeSeriesLiteral>{};
            pair.reserve(2);
            pair.push_back(scalar(dictionary->key_type(), key));
            if (is_remove(item)) {
                pair.emplace_back(literal::TimeSeriesLiteralItem{literal::REMOVE});
            } else {
                pair.push_back(delta(dictionary->value_type(), item));
            }
            values.push_back(sequence(std::move(pair)));
        });
        return sequence(std::move(values));
    }

    throw std::invalid_argument{"test_graph does not support this time-series input type"};
}

} // namespace bg::python::details

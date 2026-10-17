#include "python_literal_builder.hpp"

#include <bg/python/scalar_conversion.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>

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

PythonLiteralBuilder::PythonLiteralBuilder() : memory_{pool_} {}

literal::TimeSeriesLiteral PythonLiteralBuilder::scalar(const ScalarType &type, nb::handle value) {
    auto stored = std::make_unique<ScalarValue<>>(scalar_conversions().from_python(type, value, memory_));
    const auto &result = *stored;
    scalars_.push_back(std::move(stored));
    return literal::TimeSeriesLiteral(result);
}

literal::ScalarLiteral &PythonLiteralBuilder::removed_value(const ScalarType &type, nb::handle value) {
    auto stored = std::make_unique<ScalarValue<>>(scalar_conversions().from_python(type, value, memory_));
    auto literal_value = std::make_unique<literal::ScalarLiteral>(*stored);
    auto &result = *literal_value;
    scalars_.push_back(std::move(stored));
    removed_values_.push_back(std::move(literal_value));
    return result;
}

std::span<const literal::TimeSeriesLiteral>
PythonLiteralBuilder::sequence(std::vector<literal::TimeSeriesLiteral> values) {
    auto stored = std::make_unique<std::vector<literal::TimeSeriesLiteral>>(std::move(values));
    const auto result = std::span<const literal::TimeSeriesLiteral>{*stored};
    sequences_.push_back(std::move(stored));
    return result;
}

literal::TimeSeriesLiteral PythonLiteralBuilder::delta(const TimeSeriesType &type, nb::handle value,
                                                        QueueMode mode) {
    // Parentheses select the literal leaf constructor instead of the one-element sequence constructor.
    if (value.is_none()) {
        return literal::TimeSeriesLiteral(literal::SKIP);
    }
    if (is_invalid(value)) {
        if (!type.supports_invalidation()) {
            throw std::invalid_argument{"this time-series type does not support invalidation"};
        }
        return literal::TimeSeriesLiteral(literal::INVALID);
    }
    if (const auto *time_series = type_cast<TimeSeriesValueType>(&type)) {
        if (mode == QueueMode::batch) {
            const auto *list = type_cast<ListScalarType>(&time_series->value_type());
            if (list == nullptr) {
                throw std::invalid_argument{"batch queue leaves require a scalar list"};
            }
            return scalar(list->item_type(), value);
        }
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
                removed_markers_.push_back(
                    std::make_unique<literal::TaggedLiteral>(literal::REMOVED(removed_value(set->item_type(), item))));
                values.emplace_back(*removed_markers_.back());
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
        return literal::TimeSeriesLiteral(sequence(std::move(values)));
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
            pair.push_back(delta(list->item_type(static_cast<std::size_t>(index)), item, mode));
            values.push_back(literal::TimeSeriesLiteral(sequence(std::move(pair))));
        });
        return literal::TimeSeriesLiteral(sequence(std::move(values)));
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
            pair.push_back(delta(bundle->item_type(*index), item, mode));
            values.push_back(literal::TimeSeriesLiteral(sequence(std::move(pair))));
        });
        return literal::TimeSeriesLiteral(sequence(std::move(values)));
    }

    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        auto values = std::vector<literal::TimeSeriesLiteral>{};
        for_each_mapping_item(value, [&](nb::handle key, nb::handle item) {
            auto pair = std::vector<literal::TimeSeriesLiteral>{};
            pair.reserve(2);
            pair.push_back(scalar(dictionary->key_type(), key));
            if (is_remove(item)) {
                pair.emplace_back(literal::REMOVE);
            } else {
                pair.push_back(delta(dictionary->value_type(), item, mode));
            }
            values.push_back(literal::TimeSeriesLiteral(sequence(std::move(pair))));
        });
        return literal::TimeSeriesLiteral(sequence(std::move(values)));
    }

    throw std::invalid_argument{"unsupported Python time-series literal type"};
}

} // namespace bg::python::details

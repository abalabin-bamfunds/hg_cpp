#include "graph_run_input.hpp"

#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/run_graph.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/tuple/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include "delta.hpp"

#include <Python.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

bool is_mapping(nb::handle value) {
    return PyMapping_Check(value.ptr()) != 0 && PySequence_Check(value.ptr()) == 0;
}

bool is_dict(nb::handle value) {
    return PyDict_Check(value.ptr()) != 0;
}

nb::object mapping_items(nb::handle value) {
    if (!is_mapping(value)) {
        PyErr_SetString(PyExc_TypeError, "expected a mapping");
        nb::raise_python_error();
    }

    auto items = nb::steal<nb::object>(PyMapping_Items(value.ptr()));
    if (!items) {
        nb::raise_python_error();
    }
    return items;
}

bool is_sequence(nb::handle value) {
    if (PyUnicode_Check(value.ptr()) != 0 || PyBytes_Check(value.ptr()) != 0 || PyByteArray_Check(value.ptr()) != 0) {
        return false;
    }
    return PySequence_Check(value.ptr()) != 0;
}

bool scalar_shape(const ScalarType &type, nb::handle value) {
    if (const auto *list = type_cast<ListScalarType>(&type)) {
        if (!is_sequence(value)) {
            return false;
        }
        for (const auto item : nb::borrow<nb::iterable>(value)) {
            if (!scalar_shape(list->item_type(), item)) {
                return false;
            }
        }
        return true;
    }

    if (const auto *tuple = type_cast<TupleScalarType>(&type)) {
        if ((PyTuple_Check(value.ptr()) == 0 && PyList_Check(value.ptr()) == 0) || nb::len(value) != tuple->size()) {
            return false;
        }
        const auto values = nb::cast<nb::sequence>(value);
        for (auto index = std::size_t{}; index < tuple->size(); ++index) {
            if (!scalar_shape(tuple->item_type(index), values[index])) {
                return false;
            }
        }
        return true;
    }

    if (const auto *dictionary = type_cast<DictScalarType>(&type)) {
        if (!is_mapping(value)) {
            return false;
        }
        const auto items = mapping_items(value);
        for (const auto item : nb::borrow<nb::iterable>(items)) {
            const auto pair = nb::cast<nb::tuple>(item);
            if (!scalar_shape(dictionary->key_type(), pair[0]) || !scalar_shape(dictionary->mapped_type(), pair[1])) {
                return false;
            }
        }
        return true;
    }

    if (const auto *set = type_cast<SetScalarType>(&type)) {
        if (!PySet_Check(value.ptr()) && !PyFrozenSet_Check(value.ptr())) {
            return false;
        }
        for (const auto item : nb::borrow<nb::iterable>(value)) {
            if (!scalar_shape(set->item_type(), item)) {
                return false;
            }
        }
        return true;
    }

    if (&type == &bytes_type()) {
        return PyBytes_Check(value.ptr()) != 0 || PyMemoryView_Check(value.ptr()) != 0 ||
               PyByteArray_Check(value.ptr()) != 0;
    }

    if (type_cast<HierarchicalScalarType>(&type) != nullptr) {
        if (is_dict(value)) {
            return true;
        }
        const auto *hierarchical = type_cast<HierarchicalScalarType>(&type);
        const auto *resolved = dynamic_cast<const ResolvedHierarchicalScalarType *>(hierarchical);
        if (resolved == nullptr) {
            return false;
        }
        auto python_type = scalar_conversions().python_schema_class(resolved->schema());
        return python_type && PyType_Check(python_type.ptr()) != 0 &&
               PyType_IsSubtype(Py_TYPE(value.ptr()), reinterpret_cast<PyTypeObject *>(python_type.ptr())) != 0;
    }

    if (type_cast<BuiltinScalarType>(&type) != nullptr) {
        return !is_sequence(value) && !is_mapping(value) && !PySet_Check(value.ptr()) &&
               !PyFrozenSet_Check(value.ptr());
    }

    // Registered Python object scalars accept instances of their class. Other iterables remain candidates for
    // tick sequences, while unregistered provider scalars accept their native Python value.
    const auto python_type = scalar_conversions().python_object_class(type);
    if (!python_type) {
        return true;
    }
    const auto is_instance = PyObject_IsInstance(value.ptr(), python_type.ptr());
    if (is_instance < 0) {
        nb::raise_python_error();
    }
    return is_instance != 0;
}

bool time_series_delta_shape(const TimeSeriesType &type, nb::handle value) {
    if (value.is_none() || is_invalid(value) || is_set_delta(value)) {
        return true;
    }

    if (const auto *scalar = type_cast<TimeSeriesValueType>(&type)) {
        return scalar_shape(scalar->value_type(), value);
    }
    if (type_cast<TimeSeriesListType>(&type) != nullptr || type_cast<TimeSeriesBundleType>(&type) != nullptr ||
        type_cast<TimeSeriesDictionaryType>(&type) != nullptr) {
        return is_mapping(value);
    }
    if (type_cast<TimeSeriesSetType>(&type) != nullptr) {
        return PySet_Check(value.ptr()) != 0 || PyFrozenSet_Check(value.ptr()) != 0;
    }
    return false;
}

} // namespace

GraphRunInputs::GraphRunInputs(const Signature &signature, std::span<const Type *const> wiring_parameter_types,
                               nb::sequence fixed_values, nb::sequence positional_values, nb::dict named_values)
    : ticks_type_{nb::module_::import_("bg").attr("_Ticks")} {
    const auto fixed_count = signature.first_variadic_parameter().value_or(signature.parameter_count());
    if (nb::len(fixed_values) != fixed_count) {
        throw std::invalid_argument{"run_graph received the wrong number of fixed arguments"};
    }
    if (wiring_parameter_types.size() != signature.parameter_count()) {
        throw std::invalid_argument{"run_graph wiring types do not match the graph signature"};
    }
    fixed_values_.reserve(fixed_count);
    for (auto index = std::size_t{}; index < fixed_count; ++index) {
        fixed_values_.push_back(
            prepare(fixed_values[index], *signature.parameter(index).type, *wiring_parameter_types[index]));
    }

    positional_values_.reserve(nb::len(positional_values));
    const auto positional_parameter = signature.variadic_parameter(bg::details::VariadicMode::positional);
    for (auto index = std::size_t{}; index < nb::len(positional_values); ++index) {
        if (!positional_parameter) {
            throw std::invalid_argument{"run_graph received positional values for a graph without *args"};
        }
        const auto parameter_index = *positional_parameter;
        const auto &parameter = signature.parameter(parameter_index);
        const auto *time_series_type = type_cast<TimeSeriesType>(parameter.type);
        if (time_series_type == nullptr) {
            throw std::invalid_argument{"run_graph variadic inputs must resolve to time-series types"};
        }
        const auto &item_type = parameter.variadic_operations->item_type(*time_series_type, index);
        positional_values_.push_back(prepare(positional_values[index], item_type, item_type));
    }

    named_values_.reserve(nb::len(named_values));
    const auto named_parameter = signature.variadic_parameter(bg::details::VariadicMode::named);
    for (const auto [name, value] : named_values) {
        if (!named_parameter) {
            throw std::invalid_argument{"run_graph received named values for a graph without **kwargs"};
        }
        const auto parameter_index = *named_parameter;
        const auto &parameter = signature.parameter(parameter_index);
        const auto *parameter_type = type_cast<TimeSeriesType>(parameter.type);
        if (parameter_type == nullptr) {
            throw std::invalid_argument{"run_graph variadic inputs must resolve to time-series types"};
        }
        const auto *bundle_type = type_cast<TimeSeriesBundleType>(&parameter_type->target_type());
        auto field_index = std::optional<std::size_t>{};
        if (bundle_type != nullptr && bundle_type->item_count() != std::dynamic_extent) {
            const auto field_name = util::symbol{nb::cast<std::string>(name)};
            for (auto index = std::size_t{}; index < bundle_type->item_count(); ++index) {
                if (bundle_type->item_name(index) == field_name) {
                    field_index = index;
                    break;
                }
            }
        }

        if (field_index) {
            const auto &item_type = parameter.variadic_operations->item_type(*parameter_type, *field_index);
            named_values_.emplace_back(nb::borrow<nb::object>(name), prepare(value, item_type, item_type));
        } else if (Py_TYPE(value.ptr()) != reinterpret_cast<PyTypeObject *>(ticks_type_.ptr())) {
            // A generic TSB does not declare field types. Preserve values for normal wiring.
            named_values_.emplace_back(nb::borrow<nb::object>(name),
                                       Argument{nb::borrow<nb::object>(value), std::nullopt});
        } else {
            throw std::invalid_argument{"run_graph needs a declared TSB field type for a tick input"};
        }
    }
}

const TimeSeriesType &GraphRunInputs::input_type(const Type &parameter_type, const Type &wiring_type) const {
    auto *time_series = type_cast<TimeSeriesType>(&parameter_type);
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

        throw std::invalid_argument{"run_graph needs fully resolved input types for literal inputs"};
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

    throw std::invalid_argument{"run_graph inputs must resolve to time-series types"};
}

GraphRunInputs::Argument GraphRunInputs::prepare(nb::handle value, const Type &parameter_type,
                                                  const Type &wiring_type) {
    auto force_ticks = false;
    if (Py_TYPE(value.ptr()) == reinterpret_cast<PyTypeObject *>(ticks_type_.ptr())) {
        value = nb::getattr(value, "values");
        force_ticks = true;
    }

    if (nb::isinstance<PythonWiringPoint>(value) || type_cast<ScalarType>(&parameter_type) != nullptr) {
        if (force_ticks) {
            throw std::invalid_argument{"bg.ticks requires a time-series graph input"};
        }
        return {nb::borrow<nb::object>(value), std::nullopt};
    }

    const auto &time_series_type = input_type(parameter_type, wiring_type);
    if (!force_ticks && !value.is_none() && !is_invalid(value)) {
        if (const auto *scalar = type_cast<TimeSeriesValueType>(&time_series_type);
            scalar != nullptr && scalar_shape(scalar->value_type(), value)) {
            return {nb::borrow<nb::object>(value), std::nullopt};
        }
    }

    auto literals = std::vector<literal::TimeSeriesLiteral>{};
    if (force_ticks || !time_series_delta_shape(time_series_type, value)) {
        for (const auto tick : nb::borrow<nb::iterable>(value)) {
            literals.push_back(literals_.delta(time_series_type, tick));
        }
    } else {
        literals.push_back(literals_.delta(time_series_type, value));
    }
    inputs_.emplace_back(literals_.sequence(std::move(literals)));
    inputs_.back().time_series_type = &time_series_type;
    const auto index = inputs_.size() - 1;
    input_length_ = std::max(input_length_, inputs_.back().ticks().size());
    return {nb::none(), index};
}

nb::object GraphRunInputs::argument_value(const Argument &argument) const {
    if (!argument.input_index) {
        return nb::borrow<nb::object>(argument.value);
    }
    const auto point = bg::graph_run_input(inputs_[*argument.input_index]);
    return nb::cast(PythonWiringPoint{point, PythonWiringScope::current()});
}

nb::list GraphRunInputs::fixed_values() const {
    auto result = nb::list{};
    for (const auto &argument : fixed_values_) {
        result.append(argument_value(argument));
    }
    return result;
}

nb::list GraphRunInputs::positional_values() const {
    auto result = nb::list{};
    for (const auto &argument : positional_values_) {
        result.append(argument_value(argument));
    }
    return result;
}

nb::dict GraphRunInputs::named_values() const {
    auto result = nb::dict{};
    for (const auto &[name, argument] : named_values_) {
        result[name] = argument_value(argument);
    }
    return result;
}

} // namespace bg::python::details

#include "operator_binding.hpp"

#include <bg/graph/requirement.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/python/gil.hpp>
#include <bg/python/node.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <nanobind/nanobind.h>

#include <ddv/serial_visitor.h>

#include <optional>
#include <string>
#include <string_view>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

using PYOBJECT = ScalarVariable<"PYOBJECT">;
using KEY = ScalarVariable<"KEY">;
using ITEM = operators::ITEM;

nb::object optional_attribute(nb::handle object, const char *name) {
    auto *value = PyObject_GetAttrString(object.ptr(), name);
    if (value != nullptr) {
        return nb::steal<nb::object>(value);
    }
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        return {};
    }
    nb::raise_python_error();
}

nb::object type_hints(nb::handle object) {
    auto get_type_hints = nb::module_::import_("typing").attr("get_type_hints");
    return get_type_hints(object);
}

nb::object annotation(nb::handle annotations, nb::handle name) {
    auto *value = PyObject_GetItem(annotations.ptr(), name.ptr());
    if (value != nullptr) {
        return nb::steal<nb::object>(value);
    }
    if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyErr_Clear();
        return {};
    }
    nb::raise_python_error();
}

nb::object return_annotation(nb::handle callable) {
    if (!optional_attribute(callable, "__annotations__")) {
        return {};
    }
    auto hints = type_hints(callable);
    return annotation(hints, nb::str{"return"});
}

nb::object field_annotation(nb::handle python_class, std::string_view field_name) {
    const auto name = nb::str{field_name.data(), field_name.size()};
    auto hints = type_hints(python_class);
    auto field_type = annotation(hints, name);
    if (field_type) {
        return field_type;
    }

    auto field = optional_attribute(python_class, std::string{field_name}.c_str());
    if (!field) {
        return {};
    }

    auto property_type = nb::module_::import_("builtins").attr("property");
    const auto is_property = PyObject_IsInstance(field.ptr(), property_type.ptr());
    if (is_property < 0) {
        nb::raise_python_error();
    }
    if (is_property == 0) {
        return {};
    }

    auto getter = optional_attribute(field, "fget");
    return getter ? return_annotation(getter) : nb::object{};
}

const TimeSeriesValueType *python_object_input_type(const WiringArguments &arguments, std::string_view name) {
    const auto *time_series = type_cast<TimeSeriesType>(&arguments.resolved_type(name));
    if (time_series == nullptr) {
        return nullptr;
    }
    return type_cast<TimeSeriesValueType>(&time_series->reference_free_type());
}

nb::object python_object_input_class(const WiringArguments &arguments, std::string_view name) {
    const auto *input = python_object_input_type(arguments, name);
    if (input == nullptr) {
        return {};
    }
    return scalar_conversions().python_object_class(input->value_type());
}

const ScalarType *scalar_type_from_annotation(nb::handle annotation) {
    if (!annotation || !PyType_Check(annotation.ptr())) {
        return nullptr;
    }
    return &scalar_conversions().type_from_python(annotation);
}

const TimeSeriesType *time_series_type_from_annotation(nb::handle annotation) {
    const auto *scalar_type = scalar_type_from_annotation(annotation);
    return scalar_type == nullptr ? nullptr
                                  : &time_series_value_type(*scalar_type, TimeSeriesGuarantee::ordinary);
}

const TimeSeriesType *resolve_getitem_type(const WiringArguments &arguments) {
    auto python_class = python_object_input_class(arguments, "collection");
    if (!python_class) {
        return nullptr;
    }

    auto method = optional_attribute(python_class, "__getitem__");
    if (!method) {
        return nullptr;
    }
    return time_series_type_from_annotation(return_annotation(method));
}

std::optional<std::string> scalar_attribute_name(const WiringArguments &arguments) {
    const auto value = arguments.scalar("name");
    if (!value) {
        return std::nullopt;
    }

    if (&value->type() == &symbol_type()) {
        const auto name = scalar_cast<symbol>(*value).value();
        if (!name.has_value()) {
            return std::nullopt;
        }
        return std::string{name.text()};
    }
    if (&value->type() == &string_type()) {
        return std::string{scalar_cast<string>(*value).value()};
    }
    return std::nullopt;
}

const TimeSeriesType *resolve_getattr_type(const WiringArguments &arguments) {
    const auto name = scalar_attribute_name(arguments);
    if (!name) {
        return nullptr;
    }

    auto python_class = python_object_input_class(arguments, "bundle");
    if (!python_class) {
        return nullptr;
    }
    return time_series_type_from_annotation(field_annotation(python_class, *name));
}

RequirementResult python_object_output_requirement(const WiringArguments &arguments, std::string_view input_name) {
    if (!python_object_input_class(arguments, input_name)) {
        return reject("the input must be a Python-object scalar time series");
    }

    const auto *output = type_cast<TimeSeriesValueType>(arguments.resolved_output_type());
    if (output == nullptr) {
        return reject("the output must be a scalar time series");
    }
    if (!scalar_conversions().can_from_python(output->value_type())) {
        return reject("the output scalar type has no Python conversion");
    }
    return accept;
}

void publish_python_result(const Out<ITEM> &output, nb::handle value, EvaluationContext *context) {
    const auto &runtime_type = static_cast<const TimeSeriesValueRuntimeType &>(output.runtime_type());
    auto &memory = context->runtime_graph().allocation();
    auto result = scalar_conversions().from_python(runtime_type.scalar_runtime_type().type(), value, memory);
    output.visit(
        [&](Out<TS<SCALAR>> *typed_output) { typed_output->set_value(std::move(result)); }, ddv::noop);
}

struct python_object_getitem_scalar_key {
    In<TS<PYOBJECT>> collection;
    Scalar<KEY> key;
    Out<ITEM> output;

    static const TimeSeriesType *resolve(ITEM, const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        return resolve_getitem_type(arguments);
    }

    static RequirementResult require(const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        return python_object_output_requirement(arguments, "collection");
    }

    void evaluate(EvaluationContext *context) const {
        const auto collection_value = collection.value_view();
        const auto object = scalar_conversions().to_python(Scalar<>::from(collection_value));
        const auto index = scalar_conversions().to_python(Scalar<>::from(key));
        auto result = nb::steal<nb::object>(PyObject_GetItem(object.ptr(), index.ptr()));
        if (!result) {
            nb::raise_python_error();
        }
        publish_python_result(output, result, context);
    }
};

struct python_object_getitem_time_series_key {
    In<TS<PYOBJECT>> collection;
    In<TS<KEY>> key;
    Out<ITEM> output;

    static const TimeSeriesType *resolve(ITEM, const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        return resolve_getitem_type(arguments);
    }

    static RequirementResult require(const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        return python_object_output_requirement(arguments, "collection");
    }

    void evaluate(EvaluationContext *context) const {
        const auto collection_value = collection.value_view();
        const auto key_value = key.value_view();
        const auto object = scalar_conversions().to_python(Scalar<>::from(collection_value));
        const auto index = scalar_conversions().to_python(Scalar<>::from(key_value));
        auto result = nb::steal<nb::object>(PyObject_GetItem(object.ptr(), index.ptr()));
        if (!result) {
            nb::raise_python_error();
        }
        publish_python_result(output, result, context);
    }
};

struct python_object_getattr {
    In<TS<PYOBJECT>> bundle;
    Scalar<SCALAR> name;
    Out<ITEM> output;

    static const TimeSeriesType *resolve(ITEM, const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        return resolve_getattr_type(arguments);
    }

    static RequirementResult require(const WiringArguments &arguments) {
        const auto gil = GilScope{};
        gil.check_error();
        if (!scalar_attribute_name(arguments)) {
            return reject("the attribute name must be a string or symbol scalar");
        }
        return python_object_output_requirement(arguments, "bundle");
    }

    void evaluate(EvaluationContext *context) const {
        const auto bundle_value = bundle.value_view();
        const auto object = scalar_conversions().to_python(Scalar<>::from(bundle_value));
        const auto attribute = scalar_conversions().to_python(Scalar<>::from(name));
        auto result = nb::steal<nb::object>(PyObject_GetAttr(object.ptr(), attribute.ptr()));
        if (!result) {
            nb::raise_python_error();
        }
        publish_python_result(output, result, context);
    }
};

} // namespace

void register_python_object_operators() {
    operators::getitem.overload(bg::python::node<python_object_getitem_scalar_key>());
    operators::getitem.overload(bg::python::node<python_object_getitem_time_series_key>());
    operators::getattr.overload(bg::python::node<python_object_getattr>());
}

} // namespace bg::python::details

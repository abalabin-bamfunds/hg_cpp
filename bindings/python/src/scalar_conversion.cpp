#include <bg/python/scalar_conversion.hpp>

#include <bg/python/object.hpp>

#include <bg/scalar/builtins.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>

#include <nanobind/stl/string.h>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace bg::python {
namespace {

template <typename T, typename Source> ScalarValue<> make_scalar(Source &&value, allocator &memory) {
    auto typed_value = ScalarValue<T>{std::forward<Source>(value), memory};
    return ScalarValue<>{typed_value, memory};
}

ScalarValue<> from_python_bool(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    if (!PyBool_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected bool");
        nanobind::raise_python_error();
    }
    return make_scalar<bool>(value.ptr() == Py_True, memory);
}

nanobind::object to_python_byte(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::int_{std::to_integer<unsigned int>(scalar_cast<byte>(value).value())};
}
ScalarValue<> from_python_byte(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    const auto converted = nanobind::cast<int>(value);
    if (converted < 0 || converted > 255) {
        throw std::out_of_range{"a Python integer for bg.byte must be in [0, 255]"};
    }
    return make_scalar<byte>(static_cast<byte>(converted), memory);
}

ScalarValue<> from_python_i64(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    if (!PyLong_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected an integer for bg.i64");
        nanobind::raise_python_error();
    }

    const auto converted = PyLong_AsLongLong(value.ptr());
    if (converted == -1 && PyErr_Occurred() != nullptr) {
        nanobind::raise_python_error();
    }
    return make_scalar<i64>(static_cast<i64>(converted), memory);
}

ScalarValue<> from_python_double(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    return make_scalar<double>(nanobind::cast<double>(value), memory);
}

ScalarValue<> from_python_string(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    return make_scalar<string>(nanobind::cast<std::string>(value), memory);
}

ScalarValue<> from_python_symbol(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    return make_scalar<symbol>(nanobind::cast<symbol>(value), memory);
}

nanobind::object to_python_list(const Scalar<List<SCALAR>> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::list{};
    for (auto index = std::size_t{}; index < value.size(); ++index) {
        const auto item = value[index];
        result.append(conversions.to_python(Scalar<>::from(item)));
    }
    return result;
}

nanobind::object to_python_set(const Scalar<Set<SCALAR>> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::steal<nanobind::object>(PySet_New(nullptr));
    if (!result) {
        nanobind::raise_python_error();
    }

    value.for_each([&](Scalar<SCALAR> item) {
        const auto converted = conversions.to_python(Scalar<>::from(item));
        if (PySet_Add(result.ptr(), converted.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    });
    return result;
}

nanobind::object to_python_dict(const Scalar<Dict<SCALAR, SCALAR>> &value,
                               const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    value.for_each([&](Scalar<SCALAR> key, Scalar<SCALAR> item) {
        const auto converted_key = conversions.to_python(Scalar<>::from(key));
        const auto converted_value = conversions.to_python(Scalar<>::from(item));
        if (PyDict_SetItem(result.ptr(), converted_key.ptr(), converted_value.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    });
    return result;
}

nanobind::object to_python_struct(const Scalar<STRUCT> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    for (auto index = std::size_t{}; index < value.field_count(); ++index) {
        const auto name = value.field_name(index);
        const auto key = nanobind::str{name.data(), name.size()};
        const auto item = value[index];
        const auto field = conversions.to_python(Scalar<>::from(item));
        if (PyDict_SetItem(result.ptr(), key.ptr(), field.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    }
    return result;
}

nanobind::object to_python_hierarchical(const Scalar<HIERARCHICAL> &value,
                                       const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    for (auto index = std::size_t{}; index < value.field_count(); ++index) {
        const auto name = value.field_name(index);
        const auto key = nanobind::str{name.data(), name.size()};
        auto field = nanobind::object{nanobind::none()};
        if (value.present(index)) {
            const auto item = value[index];
            field = conversions.to_python(Scalar<>::from(item));
        }
        if (PyDict_SetItem(result.ptr(), key.ptr(), field.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    }
    return result;
}

} // namespace

struct NB_HIDDEN PythonScalarConversions::Impl {
    struct PythonType {
        Object type;
        const ScalarType *scalar_type;
    };

    ScalarDispatch<PythonToScalar> from_python;
    ScalarDispatch<ScalarToPython> to_python;
    std::unordered_map<PyTypeObject *, PythonType> python_types;
};

PythonScalarConversions::PythonScalarConversions() : impl_{std::make_unique<Impl>()} {
    const auto add_builtin = [this](const ScalarType &type, PythonToScalar conversion) {
        if (!impl_->from_python.add(type, conversion)) {
            throw std::logic_error{"a built-in Python scalar conversion was registered twice"};
        }
    };

    add_builtin(byte_type(), &from_python_byte);
    add_builtin(bool_type(), &from_python_bool);
    add_builtin(int64_type(), &from_python_i64);
    add_builtin(double_type(), &from_python_double);
    add_builtin(string_type(), &from_python_string);
    add_builtin(symbol_type(), &from_python_symbol);

    const auto add_python_builtin = [this](PyTypeObject &python_type, const ScalarType &scalar_type) {
        auto type = nanobind::handle{reinterpret_cast<PyObject *>(&python_type)};
        impl_->python_types.emplace(&python_type, Impl::PythonType{Object{type}, &scalar_type});
    };

    add_python_builtin(PyBool_Type, bool_type());
    add_python_builtin(PyLong_Type, int64_type());
    add_python_builtin(PyFloat_Type, double_type());
    add_python_builtin(PyUnicode_Type, string_type());
}

PythonScalarConversions::~PythonScalarConversions() = default;

bool PythonScalarConversions::add(const ScalarType &type, PythonToScalar conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a Python-to-scalar conversion cannot be null"};
    }
    return impl_->from_python.add(type, conversion);
}

bool PythonScalarConversions::add(const ScalarType &type, ScalarToPython conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python conversion cannot be null"};
    }
    return impl_->to_python.add(type, conversion);
}

bool PythonScalarConversions::add(const ScalarRuntimeType &runtime_type, ScalarToPython conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python conversion cannot be null"};
    }
    return impl_->to_python.add(runtime_type, conversion);
}

bool PythonScalarConversions::add_python_type(nanobind::handle python_type, const ScalarType &type) {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"a Python scalar type registration requires a Python type"};
    }

    auto *type_key = reinterpret_cast<PyTypeObject *>(python_type.ptr());
    return impl_->python_types.emplace(type_key, Impl::PythonType{Object{python_type}, &type}).second;
}

nanobind::object PythonScalarConversions::to_python(Scalar<> value) const {
    auto result = value.visit(
        [this](const Scalar<byte> *scalar) -> nanobind::object {
            return to_python_byte(Scalar<>::from(*scalar), *this);
        },
        [](const Scalar<bool> *scalar) -> nanobind::object { return nanobind::bool_{scalar->value()}; },
        [](const Scalar<i64> *scalar) -> nanobind::object { return nanobind::int_{scalar->value()}; },
        [](const Scalar<double> *scalar) -> nanobind::object { return nanobind::float_{scalar->value()}; },
        [](const Scalar<date_time_t> *) -> nanobind::object {
            throw std::invalid_argument{"Python conversion for bg.date_time_t is not registered"};
        },
        [](const Scalar<timedelta_t> *) -> nanobind::object {
            throw std::invalid_argument{"Python conversion for bg.timedelta_t is not registered"};
        },
        [](const Scalar<string> *scalar) -> nanobind::object {
            const auto text = scalar->value();
            return nanobind::str{text.data(), text.size()};
        },
        [](const Scalar<symbol> *scalar) -> nanobind::object { return nanobind::cast(scalar->value()); },
        [this](const Scalar<List<SCALAR>> *scalar) -> nanobind::object { return to_python_list(*scalar, *this); },
        [this](const Scalar<Set<SCALAR>> *scalar) -> nanobind::object { return to_python_set(*scalar, *this); },
        [this](const Scalar<Dict<SCALAR, SCALAR>> *scalar) -> nanobind::object {
            return to_python_dict(*scalar, *this);
        },
        [this](const Scalar<STRUCT> *scalar) -> nanobind::object { return to_python_struct(*scalar, *this); },
        [this](const Scalar<HIERARCHICAL> *scalar) -> nanobind::object {
            return to_python_hierarchical(*scalar, *this);
        },
        [this](const Scalar<> *scalar) -> nanobind::object {
            const auto *conversion = impl_->to_python.find(scalar->runtime_type());
            if (conversion == nullptr) {
                throw std::invalid_argument{"no Python conversion is registered for this scalar runtime type"};
            }
            return (*conversion)(*scalar, *this);
        },
        ddv::noop);
    if (!result) {
        throw std::invalid_argument{"no Python conversion is registered for this scalar value"};
    }
    return std::move(*result);
}

const ScalarType &PythonScalarConversions::type_from_python(nanobind::handle python_type) const {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"Python scalar type inference requires a Python type"};
    }

    auto method_resolution_order =
        nanobind::steal<nanobind::object>(PyObject_GetAttrString(python_type.ptr(), "__mro__"));
    if (!method_resolution_order) {
        nanobind::raise_python_error();
    }

    const auto count = PyTuple_GET_SIZE(method_resolution_order.ptr());
    for (auto index = Py_ssize_t{}; index < count; ++index) {
        auto *type = reinterpret_cast<PyTypeObject *>(PyTuple_GET_ITEM(method_resolution_order.ptr(), index));
        const auto found = impl_->python_types.find(type);
        if (found != impl_->python_types.end()) {
            return *found->second.scalar_type;
        }
    }

    throw std::invalid_argument{"no scalar type is registered for this Python type"};
}

ScalarValue<> PythonScalarConversions::from_python(nanobind::handle value, allocator &memory) const {
    if (value.ptr() == nullptr) {
        throw std::invalid_argument{"a Python scalar value cannot be null"};
    }

    const auto &type = type_from_python(nanobind::handle{reinterpret_cast<PyObject *>(Py_TYPE(value.ptr()))});
    return from_python(type, value, memory);
}

ScalarValue<> PythonScalarConversions::from_python(const ScalarType &type, nanobind::handle value,
                                                   allocator &memory) const {
    const auto *conversion = impl_->from_python.find(type);
    if (conversion == nullptr) {
        throw std::invalid_argument{"no Python-to-scalar conversion is registered for this scalar type"};
    }
    return (*conversion)(value, memory, *this);
}

bool PythonScalarConversions::can_from_python(const ScalarType &type) const noexcept {
    return impl_->from_python.find(type) != nullptr;
}

PythonToScalar PythonScalarConversions::from_python_handler(const ScalarType &type) const noexcept {
    const auto *conversion = impl_->from_python.find(type);
    return conversion == nullptr ? nullptr : *conversion;
}

PythonScalarConversions &scalar_conversions() {
    static auto conversions = PythonScalarConversions{};
    return conversions;
}

} // namespace bg::python

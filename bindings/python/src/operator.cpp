#include <bg/python/operator.hpp>

#include "operator_binding.hpp"
#include "wiring_argument.hpp"

#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/operators.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>

#include <nanobind/stl/string.h>

#include <array>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace bg::python {
namespace details {

PythonOperator::PythonOperator(const Operator &operation) : operation_{&operation} {}

PythonOperator::PythonOperator(const Operator &operation,
                               std::span<const Type *const> type_parameters)
    : operation_{&operation}, type_parameters_{type_parameters.begin(), type_parameters.end()} {}

PythonOperator::PythonOperator(const PythonOperator &other)
    : operation_{other.operation_}, type_parameters_{other.type_parameters_}, type_map_{other.type_map_.copy()} {}

PythonOperator &PythonOperator::operator=(const PythonOperator &other) {
    if (this != &other) {
        operation_ = other.operation_;
        type_parameters_ = other.type_parameters_;
        type_map_ = other.type_map_.copy();
    }
    return *this;
}

nb::object PythonOperator::call(nb::args positional, nb::kwargs named) const {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(positional.size() + named.size());

    for (const auto value : positional) {
        arguments.push_back(wiring_argument(value, memory, wiring));
    }

    auto position = Py_ssize_t{};
    PyObject *name{};
    PyObject *value{};
    while (PyDict_Next(named.ptr(), &position, &name, &value)) {
        auto name_size = Py_ssize_t{};
        const auto *name_text = PyUnicode_AsUTF8AndSize(name, &name_size);
        if (name_text == nullptr) {
            nb::raise_python_error();
        }

        arguments.emplace_back(util::symbol{std::string_view{name_text, static_cast<std::size_t>(name_size)}},
                               wiring_argument(nb::handle{value}, memory, wiring));
    }

    return invoke(arguments);
}

nb::object PythonOperator::call(nb::handle lhs, nb::handle rhs) const {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    const auto arguments = std::array{wiring_argument(lhs, memory, wiring), wiring_argument(rhs, memory, wiring)};
    return invoke(arguments);
}

nb::object PythonOperator::call(nb::handle value) const {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    const auto arguments = std::array{wiring_argument(value, memory, wiring)};
    return invoke(arguments);
}

nb::object PythonOperator::invoke(std::span<const WiringArgument> arguments) const {
    try {
        const auto result = static_cast<const WiringTemplate &>(*operation_)(type_map_, arguments);
        if (!result) {
            return nb::none();
        }
        return nb::cast(PythonWiringPoint{*result, PythonWiringScope::current()});
    } catch (const nb::python_error &) {
        throw;
    } catch (const std::exception &error) {
        throw std::invalid_argument{"cannot wire operator '" + name() + "': " + error.what()};
    }
}

PythonOperator PythonOperator::map_type_arguments(nb::handle arguments) const {
    auto values = std::vector<const Type *>{};
    const auto tuple = PyTuple_Check(arguments.ptr());
    const auto size = tuple ? PyTuple_GET_SIZE(arguments.ptr()) : Py_ssize_t{1};
    values.reserve(static_cast<std::size_t>(size));
    for (auto index = Py_ssize_t{}; index < size; ++index) {
        const auto value = tuple ? nb::handle{PyTuple_GET_ITEM(arguments.ptr(), index)} : arguments;
        const auto &type = python_type_argument(value);
        if (!type.is_resolved()) {
            throw std::invalid_argument{"an explicit operator type argument must be resolved"};
        }
        values.push_back(&type);
    }

    if (values.size() > type_parameters_.size()) {
        throw std::invalid_argument{"too many positional type arguments for this operator"};
    }
    if (values.empty()) {
        throw std::invalid_argument{"an operator type argument list cannot be empty"};
    }

    auto type_map = type_map_.copy();
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (!type_parameters_[index]->match(*values[index], type_map)) {
            auto parameter_name = std::string{};
            type_parameters_[index]->format_name(parameter_name);
            throw std::invalid_argument{"type argument does not match operator parameter '" + parameter_name + "'"};
        }
    }

    auto result = PythonOperator{*operation_, std::span<const Type *const>{type_parameters_}};
    result.type_map_ = std::move(type_map);
    return result;
}

std::string PythonOperator::name() const { return std::string{operation_->signature().name().text()}; }

} // namespace details

void export_operator(nb::module_ &module, std::string_view python_name, const Operator &operation) {
    const auto name = std::string{python_name};
    module.attr(name.c_str()) = nb::cast(details::PythonOperator{operation});
}

void export_operator(nb::module_ &module, std::string_view python_name, const Operator &operation,
                     std::span<const Type *const> type_parameters) {
    const auto name = std::string{python_name};
    module.attr(name.c_str()) = nb::cast(details::PythonOperator{operation, type_parameters});
}

namespace details {

void bind_operators(nb::module_ &module) {
    nb::class_<PythonOperator>(module, "_Operator")
        .def("__call__", static_cast<nb::object (PythonOperator::*)(nb::args, nb::kwargs) const>(
                             &PythonOperator::call))
        .def("__getitem__", &PythonOperator::map_type_arguments)
        .def_prop_ro("name", &PythonOperator::name)
        .def("__repr__", [](const PythonOperator &operation) { return "operator_(" + operation.name() + ")"; });

    auto operators_module = module.def_submodule("operators", "Graph operators");
    export_operator(operators_module, "add", operators::add);
    export_operator(operators_module, "sub", operators::sub);
    export_operator(operators_module, "mul", operators::mul);
    export_operator(operators_module, "div", operators::div);
    export_operator(operators_module, "floordiv", operators::floordiv);
    export_operator(operators_module, "mod", operators::mod);
    export_operator(operators_module, "pow", operators::pow);
    export_operator(operators_module, "round", operators::round);
    export_operator(operators_module, "convert", operators::convert);
    export_operator(operators_module, "pos", operators::pos);
    export_operator(operators_module, "neg", operators::neg);
    export_operator(operators_module, "abs", operators::abs);
    export_operator(operators_module, "eq", operators::eq);
    export_operator(operators_module, "ne", operators::ne);
    export_operator(operators_module, "lt", operators::lt);
    export_operator(operators_module, "le", operators::le);
    export_operator(operators_module, "gt", operators::gt);
    export_operator(operators_module, "ge", operators::ge);
    export_operator(operators_module, "and_", operators::and_);
    export_operator(operators_module, "or_", operators::or_);
    export_operator(operators_module, "not_", operators::not_);
    export_operator(operators_module, "valid", operators::valid);
    export_operator(operators_module, "all_valid", operators::all_valid);
    export_operator(operators_module, "count", operators::count);
    export_operator(operators_module, "sample", operators::sample);
    export_operator(operators_module, "dedup", operators::dedup);
    export_operator(operators_module, "filter_", operators::filter_);
    export_operator(operators_module, "if_then_else", operators::if_then_else);
    export_operator(operators_module, "lag", operators::lag);
    export_operator(operators_module, "sums", operators::sums);
    export_operator(operators_module, "mins", operators::mins);
    export_operator(operators_module, "maxs", operators::maxs);
    export_operator(operators_module, "means", operators::means);
    export_operator(operators_module, "len_", operators::len_);
    export_operator(operators_module, "contains_", operators::contains_);
    export_operator(operators_module, "const_", operators::const_);
    export_operator(operators_module, "default_", operators::default_);
    export_operator(operators_module, "getitem", operators::getitem);
    export_operator(operators_module, "getattr", operators::getattr);
}

nb::object getitem(const PythonWiringPoint &collection, nb::handle key) {
    auto &wiring = WiringContext::current().wiring();
    const auto point = collection.get(wiring);
    if (const auto *record = bg::details::variadic_wiring_record(point)) {
        const auto index = [&]() -> std::size_t {
            if (PyLong_Check(key.ptr())) {
                return nb::cast<std::size_t>(key);
            }

            const auto name = [&] {
                if (PyUnicode_Check(key.ptr())) {
                    const auto &type = scalar_conversions().type_from_python(
                        nb::handle{reinterpret_cast<PyObject *>(Py_TYPE(key.ptr()))});
                    if (&type == &symbol_type() && nb::cast<bool>(key.attr("_is_null"))) {
                        return util::symbol{};
                    }
                }
                return util::symbol{nb::cast<std::string>(key)};
            }();
            for (auto candidate = std::size_t{}; candidate < record->size(); ++candidate) {
                if (record->item_name(candidate) == name) {
                    return candidate;
                }
            }
            throw nb::key_error();
        }();
        if (index >= record->size()) {
            throw nb::index_error();
        }

        const auto argument = record->argument(index);
        if (argument.is_time_series()) {
            return nb::cast(PythonWiringPoint{argument.time_series(), PythonWiringScope::current(),
                                              argument.requires_optional()});
        }
        if (!argument.present()) {
            return nb::none();
        }
        return scalar_conversions().to_python(argument.scalar());
    }

    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(2);
    arguments.emplace_back(collection.argument(wiring));
    arguments.push_back(wiring_argument(key, memory, wiring));

    return PythonOperator{operators::getitem}.invoke(arguments);
}

} // namespace details
} // namespace bg::python

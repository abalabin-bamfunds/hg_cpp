#include <bg/python/operator.hpp>

#include "operator_binding.hpp"
#include "wiring_argument.hpp"

#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/operators.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>

#include <nanobind/stl/string.h>

#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace bg::python {
namespace {

class PythonOperator {
  public:
    explicit PythonOperator(const Operator &operation) noexcept : operation_{&operation} {}

    nb::object call(nb::args positional, nb::kwargs named) const {
        auto &wiring = WiringContext::current().wiring();
        auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
        auto memory = allocator{temporary_memory};
        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(positional.size() + named.size());

        for (const auto value : positional) {
            arguments.push_back(details::wiring_argument(value, memory, wiring));
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
                                   details::wiring_argument(nb::handle{value}, memory, wiring));
        }

        return invoke(arguments);
    }

    nb::object invoke(std::span<const WiringArgument> arguments) const {
        try {
            const auto result = static_cast<const WiringTemplate &>(*operation_)(arguments);
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

    std::string name() const { return std::string{operation_->signature().name().text()}; }

  private:
    const Operator *operation_;
};

} // namespace

void export_operator(nb::module_ &module, std::string_view python_name, const Operator &operation) {
    const auto name = std::string{python_name};
    module.attr(name.c_str()) = nb::cast(PythonOperator{operation});
}

namespace details {

void bind_operators(nb::module_ &module) {
    nb::class_<PythonOperator>(module, "_Operator")
        .def("__call__", &PythonOperator::call)
        .def_prop_ro("name", &PythonOperator::name)
        .def("__repr__", [](const PythonOperator &operation) { return "operator_(" + operation.name() + ")"; });

    auto operators_module = module.def_submodule("operators", "Graph operators");
    export_operator(operators_module, "const_", operators::const_);
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

            const auto name = nb::isinstance<util::symbol>(key) ? nb::cast<util::symbol>(key)
                                                                : util::symbol{nb::cast<std::string>(key)};
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
            return nb::cast(PythonWiringPoint{argument.time_series(), PythonWiringScope::current()});
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
    arguments.emplace_back(point);
    arguments.push_back(wiring_argument(key, memory, wiring));

    return PythonOperator{operators::getitem}.invoke(arguments);
}

} // namespace details
} // namespace bg::python

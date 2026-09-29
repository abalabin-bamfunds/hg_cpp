#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/python/operator.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>

namespace {

struct list_source_definition {
    bg::Out<bg::TSL<bg::TS<bg::i64>, 2>> output;

    void start() const {
        output[0].set(10);
        output[1].set(20);
    }

    void evaluate() const {}
};

const bg::Operator &discard_operator() {
    static auto operation = bg::operator_<void(bg::TS_T)>{"discard", {"value"}};
    static const auto &implementation = operation.overload(bg::node([](bg::In<bg::TS_T>) {}));
    (void)implementation;

    return operation;
}

} // namespace

NB_MODULE(_bg_test_extension, module) {
    bg::python::export_operator(module, "const_", bg::operators::const_);
    bg::python::export_operator(module, "discard", discard_operator());

    module.def("make_list", [] {
        static const auto source = bg::node<list_source_definition>();
        const auto point = source();
        return nanobind::cast(bg::python::PythonWiringPoint{point, bg::python::PythonWiringScope::current()});
    });
    module.def("register_double_type", [](nanobind::handle python_type) {
        return bg::python::scalar_conversions().add_python_type(python_type, bg::double_type());
    });
    module.def("scalar_conversions_address",
               [] { return reinterpret_cast<std::uintptr_t>(&bg::python::scalar_conversions()); });
}

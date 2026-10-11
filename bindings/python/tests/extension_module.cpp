#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/python/input_view.hpp>
#include <bg/python/operator.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/type/scalar_type.hpp>

#include <nanobind/nanobind.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

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

const bg::Operator &invalidate_on_zero_operator() {
    static auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"invalidate_on_zero", {"value"}};
    static const auto &implementation = operation.overload(bg::node([](bg::In<bg::TS<bg::i64>> input,
                                                                       bg::Out<bg::TS<bg::i64>> output) {
        if (input.value() == 0) {
            output.invalidate();
            return;
        }

        output.set(input.value());
    }));
    (void)implementation;

    return operation;
}

const bg::Operator &optional_choice_operator() {
    static auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS_T>{"optional_choice", {"value"}};
    static const auto &required = operation.overload(bg::node([](bg::In<bg::TS<bg::i64>> input,
                                                                 bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value() + 10);
    }));
    static const auto &optional = operation.overload(bg::node([](bg::In<bg::TS<bg::i64>, bg::optional> input,
                                                                 bg::Out<bg::TS<bg::i64>> output) {
        output.set((input.valid() ? input.value() : 0) + 20);
    }));
    (void)required;
    (void)optional;

    return operation;
}

nanobind::object runtime_double_to_python(bg::Scalar<>, const bg::python::PythonScalarConversions &) {
    return nanobind::str{"runtime double conversion"};
}

nanobind::object runtime_double_to_python_view(bg::Scalar<>, const bg::python::PythonScalarConversions &,
                                               const std::shared_ptr<bg::python::CallbackLifetime> &,
                                               std::uint64_t) {
    return nanobind::str{"runtime double view conversion"};
}

nanobind::object extension_scalar_to_python(bg::Scalar<>, const bg::python::PythonScalarConversions &) {
    return nanobind::str{"extension scalar conversion"};
}

} // namespace

NB_MODULE(_bg_test_extension, module) {
    bg::python::export_operator(module, "const_", bg::operators::const_);
    bg::python::export_operator(module, "discard", discard_operator());
    bg::python::export_operator(module, "invalidate_on_zero", invalidate_on_zero_operator());
    bg::python::export_operator(module, "optional_choice", optional_choice_operator());

    module.def("make_list", [] {
        static const auto source = bg::node<list_source_definition>();
        const auto point = source();
        return nanobind::cast(bg::python::PythonWiringPoint{point, bg::python::PythonWiringScope::current()});
    });
    module.def("register_double_type", [](nanobind::handle python_type) {
        return bg::python::scalar_conversions().add_python_type(python_type, bg::double_type());
    });
    module.def("copy_release_python_scalar_without_gil", [](nanobind::handle value) {
        auto memory = bg::allocator{};
        auto original = std::optional<bg::ScalarValue<>>{};
        original.emplace(bg::python::scalar_conversions().from_python(value, memory));
        {
            auto release_gil = nanobind::gil_scoped_release{};
            {
                auto copy = bg::ScalarValue<>{original->view()};
                original.reset();
            }
        }
        bg::python::Object::drain_deferred_releases();
        return true;
    });
    module.def("runtime_conversion_overrides_type_conversion", [] {
        auto conversions = bg::python::PythonScalarConversions{};
        const auto added = conversions.add(bg::cpp_scalar_runtime_type<double>(), &runtime_double_to_python);
        const auto view_added =
            conversions.add(bg::cpp_scalar_runtime_type<double>(), &runtime_double_to_python_view);
        const auto value = 42.0;
        const auto typed_scalar = bg::Scalar<double>::from(value);
        const auto scalar = bg::Scalar<>::from(typed_scalar);
        const auto lifetime = std::make_shared<bg::python::CallbackLifetime>();
        const auto callback = bg::python::CallbackScope{*lifetime};
        return nanobind::make_tuple(added, conversions.to_python(scalar), view_added,
                                    conversions.to_python_view(scalar, lifetime, callback.generation()));
    });
    module.def("date_time_conversions_can_be_registered", [] {
        auto conversions = bg::python::PythonScalarConversions{};
        const auto date_time_added = conversions.add(bg::evaluation_time_type(), &extension_scalar_to_python);
        const auto timedelta_added = conversions.add(bg::timedelta_type(), &extension_scalar_to_python);

        const auto date_time = bg::date_time_t{};
        const auto timedelta = bg::timedelta_t{};
        const auto typed_date_time = bg::Scalar<bg::date_time_t>::from(date_time);
        const auto typed_timedelta = bg::Scalar<bg::timedelta_t>::from(timedelta);
        const auto date_time_scalar = bg::Scalar<>::from(typed_date_time);
        const auto timedelta_scalar = bg::Scalar<>::from(typed_timedelta);
        const auto lifetime = std::make_shared<bg::python::CallbackLifetime>();
        const auto callback = bg::python::CallbackScope{*lifetime};
        return nanobind::make_tuple(date_time_added, conversions.to_python(date_time_scalar), timedelta_added,
                                    conversions.to_python(timedelta_scalar),
                                    conversions.to_python_view(date_time_scalar, lifetime, callback.generation()));
    });
    module.def("scalar_conversions_address",
               [] { return reinterpret_cast<std::uintptr_t>(&bg::python::scalar_conversions()); });
    module.def("decode_invalid_string", [] {
        const auto invalid_utf8 = std::string_view{"\xFF", 1};
        const auto value = bg::ScalarValue<bg::string>{invalid_utf8};
        return bg::python::scalar_conversions().to_python(bg::Scalar<>::from(value));
    });
}

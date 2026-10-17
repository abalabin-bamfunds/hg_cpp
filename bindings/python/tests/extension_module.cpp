#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operators.hpp>
#include <bg/python/input_view.hpp>
#include <bg/python/node.hpp>
#include <bg/python/operator.hpp>
#include <bg/python/object.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/scalar/builtins.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/type/scalar_type.hpp>

#include <nanobind/nanobind.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace {

std::array<std::atomic<int>, 4> native_node_lifecycle{};
std::array<std::atomic<std::uint64_t>, 2> native_interruption_lifecycle{};

struct native_continuous_timer_node {
    static constexpr bool uses_timed_scheduling = true;

    bg::In<bg::TS<bool>> repeat;
    bg::Out<bg::TS<bg::i64>> output;

    void start(bg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(bg::EvaluationContext *context) const {
        const auto evaluations = ++native_interruption_lifecycle[0];
        output.set(static_cast<bg::i64>(evaluations));
        if (*repeat) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void stop() const noexcept { ++native_interruption_lifecycle[1]; }
};

struct native_gil_node {
    bg::In<bg::TS<bg::i64>> input;
    bg::State<int> state;
    bg::Out<bg::TS<bg::i64>> output;

    void start() const { native_node_lifecycle[0] = PyGILState_Check(); }
    void evaluate() const { output.set(input.value() + PyGILState_Check()); }
    void on_invalidation() const {
        native_node_lifecycle[1] = PyGILState_Check();
        output.invalidate();
    }
    void stop() const noexcept { native_node_lifecycle[2] = PyGILState_Check(); }
    void finalize() const noexcept { native_node_lifecycle[3] = PyGILState_Check(); }
};

struct native_callable_gil_node {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(input.value() + PyGILState_Check()); }
};

struct native_python_exception_node {
    bg::In<bg::TS<bg::i64>> input;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const {
        PyErr_SetString(PyExc_ValueError, "native node Python failure");
        output.set(input.value());
    }
};

struct native_stop_exception_node {
    bg::In<bg::TS<bg::i64>> input;
    bg::State<int> state;

    void evaluate() const {}
    void stop() const noexcept {
        native_node_lifecycle[2] = 1;
        PyErr_SetString(PyExc_ValueError, "native stop Python failure");
    }
    void finalize() const noexcept { native_node_lifecycle[3] = 1; }
};

struct native_start_only_source_definition {
    native_start_only_source_definition()
        : signature_{bg::util::symbol{"native_start_only_source"}, {}, bg::TS<bg::i64>::type()} {}

    const bg::Signature &signature() const { return signature_; }

    static void start(bg::NodeInvocation<> invocation) {
        native_node_lifecycle[0] = PyGILState_Check();
        invocation.output().visit([](bg::Out<bg::TS<bg::SCALAR>> *output) {
            const auto value = bg::i64{42};
            output->set_value(bg::Scalar<bg::i64>::from(value));
        }, ddv::noop);
    }

    bg::Signature signature_;
};

const bg::Operator &native_gil_operator() {
    static auto operation = bg::operator_<auto(bg::TS<bg::i64>) -> bg::TS<bg::i64>>{"native_gil", {"value"}};
    static const auto &implementation = operation.overload(bg::python::node<native_gil_node>());
    (void)implementation;
    return operation;
}

const bg::Operator &native_callable_gil_operator() {
    static auto operation =
        bg::operator_<auto(bg::TS<bg::i64>) -> bg::TS<bg::i64>>{"native_callable_gil", {"value"}};
    static const auto &implementation = operation.overload(bg::python::node([](bg::In<bg::TS<bg::i64>> input,
                                                                                bg::Out<bg::TS<bg::i64>> output) {
        output.set(input.value() + PyGILState_Check());
    }));
    (void)implementation;
    return operation;
}

const bg::Operator &native_python_exception_operator() {
    static auto operation =
        bg::operator_<auto(bg::TS<bg::i64>) -> bg::TS<bg::i64>>{"native_python_exception", {"value"}};
    static const auto &implementation = operation.overload(bg::python::node<native_python_exception_node>());
    (void)implementation;
    return operation;
}

const bg::Operator &native_stop_exception_operator() {
    static auto operation = bg::operator_<void(bg::TS<bg::i64>)>{"native_stop_exception", {"value"}};
    static const auto &implementation = operation.overload(bg::python::node<native_stop_exception_node>());
    (void)implementation;
    return operation;
}

const bg::Operator &native_start_only_source_operator() {
    static auto operation = bg::operator_<auto() -> bg::TS<bg::i64>>{"native_start_only_source"};
    static const auto &implementation = operation.overload(
        bg::python::node(native_start_only_source_definition{}));
    (void)implementation;
    return operation;
}

const bg::Operator &native_continuous_timer_operator() {
    static auto operation = bg::operator_<auto(bg::TS<bool>) -> bg::TS<bg::i64>>{"native_continuous_timer",
                                                                                 {"repeat"}};
    static const auto &implementation = operation.overload(bg::node<native_continuous_timer_node>());
    (void)implementation;
    return operation;
}

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
    bg::python::export_operator(module, "native_gil", native_gil_operator());
    bg::python::export_operator(module, "native_callable_gil", native_callable_gil_operator());
    bg::python::export_operator(module, "native_python_exception", native_python_exception_operator());
    bg::python::export_operator(module, "native_stop_exception", native_stop_exception_operator());
    bg::python::export_operator(module, "native_start_only_source", native_start_only_source_operator());
    bg::python::export_operator(module, "native_continuous_timer", native_continuous_timer_operator());
    module.def("native_node_lifecycle", [] {
        return nanobind::make_tuple(native_node_lifecycle[0].load(), native_node_lifecycle[1].load(),
                                    native_node_lifecycle[2].load(), native_node_lifecycle[3].load());
    });
    module.def("reset_native_node_lifecycle", [] {
        for (auto &value : native_node_lifecycle) {
            value = 0;
        }
    });
    module.def("native_interruption_lifecycle", [] {
        return nanobind::make_tuple(native_interruption_lifecycle[0].load(),
                                    native_interruption_lifecycle[1].load());
    });
    module.def("reset_native_interruption_lifecycle", [] {
        for (auto &value : native_interruption_lifecycle) {
            value = 0;
        }
    });

    module.def("make_list", [] {
        static const auto source = bg::node<list_source_definition>();
        const auto point = source();
        return nanobind::cast(bg::python::PythonWiringPoint{point, bg::python::PythonWiringScope::current()});
    });
    module.def("register_double_type", [](nanobind::handle python_type) {
        return bg::python::scalar_conversions().add_python_type(python_type, bg::double_type());
    });
    module.def("register_runtime_double_result_conversion", [] {
        return bg::python::scalar_conversions().add(bg::cpp_scalar_runtime_type<double>(),
                                                    &extension_scalar_to_python);
    });
    module.def("can_convert_python_type", [](nanobind::handle python_type) {
        auto &conversions = bg::python::scalar_conversions();
        return conversions.can_from_python(conversions.type_from_python(python_type));
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
    module.def("convert_timedelta", [](nanobind::handle value) {
        auto memory = bg::allocator{};
        auto &conversions = bg::python::scalar_conversions();
        const auto converted = conversions.from_python(bg::timedelta_type(), value, memory);
        const auto view = converted.view();
        return conversions.to_python(bg::Scalar<>::from(view));
    });
    module.def("scalar_conversions_address",
               [] { return reinterpret_cast<std::uintptr_t>(&bg::python::scalar_conversions()); });
    module.def("decode_invalid_string", [] {
        const auto invalid_utf8 = std::string_view{"\xFF", 1};
        const auto value = bg::ScalarValue<bg::string>{invalid_utf8};
        return bg::python::scalar_conversions().to_python(bg::Scalar<>::from(value));
    });
}

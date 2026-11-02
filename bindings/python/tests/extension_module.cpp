#include <hg/node.hpp>
#include <hg/operator.hpp>
#include <hg/operators.hpp>
#include <hg/python/input_view.hpp>
#include <hg/python/gil.hpp>
#include <hg/python/node.hpp>
#include <hg/python/operator.hpp>
#include <hg/python/object.hpp>
#include <hg/python/schema.hpp>
#include <hg/python/scalar_conversion.hpp>
#include <hg/python/service.hpp>
#include <hg/python/wiring_point.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/scalar/builtins.hpp>
#include <hg/scalar/bundle/scalar_type.hpp>
#include <hg/scalar/declaration.hpp>
#include <hg/scalar/value.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>
#include <hg/time_series/time_series_type.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>
#include <hg/type/schema.hpp>
#include <hg/type/scalar_type.hpp>
#include <hg/service.hpp>
#include <hg/util/scope_exit.hpp>

#include <nanobind/nanobind.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {

struct ConflictingNativeSchemaIdentity {};

template <template <typename, auto...> typename Field> struct NativePairSchema {
    Field<hg::i64> value;
};

template <template <typename, auto...> typename Field> struct NativeOptionalSchema {
    Field<hg::i64, hg::optional> value;
};

const hg::Schema &native_pair_scalar_schema() {
    return hg::details::hierarchical_scalar_type<NativePairSchema>().schema();
}

const hg::Schema &native_pair_time_series_schema() {
    return static_cast<const hg::TimeSeriesBundleType &>(hg::TSB<NativePairSchema>::type()).schema();
}

const hg::Schema &native_optional_scalar_schema() {
    return hg::details::hierarchical_scalar_type<NativeOptionalSchema>().schema();
}

const hg::Schema &native_optional_time_series_schema() {
    return static_cast<const hg::TimeSeriesBundleType &>(hg::TSB<NativeOptionalSchema>::type()).schema();
}

const hg::Schema &conflicting_native_time_series_schema() {
    static const auto fields = std::array{hg::SchemaField{
        hg::util::symbol{"other"}, &hg::TS<hg::string>::type(), false}};
    return hg::schema(hg::util::symbol{"native.Other"},
                      hg::SchemaIdentity{std::type_index{typeid(ConflictingNativeSchemaIdentity)}}, nullptr, fields);
}

const auto native_python_service =
    hg::service<auto(hg::TS<hg::i64> value) -> hg::TS<hg::i64>>{"native_python_service", {"value"}};
const auto alternate_native_python_service =
    hg::service<auto(hg::TS<hg::i64> value) -> hg::TS<hg::i64>>{"alternate_native_python_service", {"value"}};

std::array<std::atomic<int>, 4> native_node_lifecycle{};
std::array<std::atomic<std::uint64_t>, 2> native_interruption_lifecycle{};

struct native_continuous_timer_node {
    static constexpr bool uses_timed_scheduling = true;

    hg::In<hg::TS<bool>> repeat;
    hg::Out<hg::TS<hg::i64>> output;

    void start(hg::EvaluationContext *context) const { context->schedule_now(); }

    void evaluate(hg::EvaluationContext *context) const {
        const auto evaluations = ++native_interruption_lifecycle[0];
        output.set(static_cast<hg::i64>(evaluations));
        if (*repeat) {
            context->schedule_at(context->evaluation_time() + std::chrono::microseconds{1});
        }
    }

    void stop() const noexcept { ++native_interruption_lifecycle[1]; }
};

struct native_gil_node {
    hg::In<hg::TS<hg::i64>> input;
    hg::State<int> state;
    hg::Out<hg::TS<hg::i64>> output;

    void start() const { native_node_lifecycle[0] = hg::python::GilScope::is_held(); }
    void evaluate() const { output.set(input.value() + hg::python::GilScope::is_held()); }
    void on_invalidation() const {
        native_node_lifecycle[1] = hg::python::GilScope::is_held();
        output.invalidate();
    }
    void stop() const noexcept { native_node_lifecycle[2] = hg::python::GilScope::is_held(); }
    void finalize() const noexcept { native_node_lifecycle[3] = hg::python::GilScope::is_held(); }
};

struct native_callable_gil_node {
    hg::In<hg::TS<hg::i64>> input;
    hg::Out<hg::TS<hg::i64>> output;

    void evaluate() const { output.set(input.value() + hg::python::GilScope::is_held()); }
};

struct native_python_exception_node {
    hg::In<hg::TS<hg::i64>> input;
    hg::Out<hg::TS<hg::i64>> output;

    void evaluate() const {
        PyErr_SetString(PyExc_ValueError, "native node Python failure");
        output.set(input.value());
    }
};

struct native_stop_exception_node {
    hg::In<hg::TS<hg::i64>> input;
    hg::State<int> state;

    void evaluate() const {}
    void stop() const noexcept {
        native_node_lifecycle[2] = 1;
        PyErr_SetString(PyExc_ValueError, "native stop Python failure");
    }
    void finalize() const noexcept { native_node_lifecycle[3] = 1; }
};

struct native_start_only_source_definition {
    native_start_only_source_definition()
        : signature_{hg::util::symbol{"native_start_only_source"}, {}, hg::TS<hg::i64>::type()} {}

    const hg::Signature &signature() const { return signature_; }

    static void start(hg::NodeInvocation<> invocation) {
        native_node_lifecycle[0] = hg::python::GilScope::is_held();
        invocation.output().visit([](hg::Out<hg::TS<hg::SCALAR>> *output) {
            const auto value = hg::i64{42};
            output->set_value(hg::Scalar<hg::i64>::from(value));
        }, ddv::noop);
    }

    hg::Signature signature_;
};

using PYOBJECT = hg::ScalarVariable<"PYOBJECT", hg::python::PythonObject>;

struct native_python_object_property_node {
    hg::In<hg::TS<PYOBJECT>> input;
    hg::Out<hg::TS<PYOBJECT>> output;

    void evaluate(hg::EvaluationContext *context) const {
        if (!hg::python::GilScope::is_held()) {
            throw std::logic_error{"native Python-object node evaluated without the GIL"};
        }

        auto &conversions = hg::python::scalar_conversions();
        const auto input_value = input.value_view();
        const auto object = conversions.to_python(hg::Scalar<>::from(input_value));
        auto result = nanobind::steal<nanobind::object>(PyObject_GetAttrString(object.ptr(), "native_result"));
        if (!result) {
            nanobind::raise_python_error();
        }

        const auto &runtime_type = static_cast<const hg::TimeSeriesValueRuntimeType &>(output.runtime_type());
        auto value = conversions.from_python(runtime_type.scalar_runtime_type().type(), result,
                                             context->runtime_graph().allocation());
        output.set_value(std::move(value));
    }
};

struct native_concrete_type_name_node {
    hg::In<hg::TS<hg::SCALAR>> input;
    hg::Out<hg::TS<hg::string>> output;

    void evaluate() const {
        auto gil = nanobind::gil_scoped_acquire{};
        const auto value = input.value_view();
        const auto &type = value.concrete_type();
        auto &conversions = hg::python::scalar_conversions();
        auto python_class = nanobind::object{};
        if (const auto *hierarchical_type = hg::type_cast<hg::HierarchicalScalarType>(&type)) {
            python_class = conversions.python_schema_class(
                static_cast<const hg::Schema &>(hierarchical_type->schema_type()));
        } else {
            python_class = conversions.python_object_class(type);
        }

        if (!python_class) {
            output.set(std::string_view{"Python class missing"});
            return;
        }

        auto class_name = nanobind::steal<nanobind::object>(PyObject_GetAttrString(python_class.ptr(), "__name__"));
        if (!class_name) {
            nanobind::raise_python_error();
        }

        auto name_size = Py_ssize_t{};
        const auto *name = PyUnicode_AsUTF8AndSize(class_name.ptr(), &name_size);
        if (name == nullptr) {
            nanobind::raise_python_error();
        }

        output.set(std::string_view{name, static_cast<std::size_t>(name_size)});
    }
};

const hg::Operator &native_concrete_type_name_operator() {
    static auto operation =
        hg::operator_<auto(hg::TS<hg::SCALAR>) -> hg::string>{"native_concrete_type_name", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node<native_concrete_type_name_node>());
    (void)implementation;
    return operation;
}

const hg::Operator &native_gil_operator() {
    static auto operation = hg::operator_<auto(hg::TS<hg::i64>) -> hg::TS<hg::i64>>{"native_gil", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node<native_gil_node>());
    (void)implementation;
    return operation;
}

const hg::Operator &native_callable_gil_operator() {
    static auto operation =
        hg::operator_<auto(hg::TS<hg::i64>) -> hg::TS<hg::i64>>{"native_callable_gil", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node([](hg::In<hg::TS<hg::i64>> input,
                                                                                hg::Out<hg::TS<hg::i64>> output) {
        output.set(input.value() + hg::python::GilScope::is_held());
    }));
    (void)implementation;
    return operation;
}

const hg::Operator &native_python_exception_operator() {
    static auto operation =
        hg::operator_<auto(hg::TS<hg::i64>) -> hg::TS<hg::i64>>{"native_python_exception", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node<native_python_exception_node>());
    (void)implementation;
    return operation;
}

const hg::Operator &native_stop_exception_operator() {
    static auto operation = hg::operator_<void(hg::TS<hg::i64>)>{"native_stop_exception", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node<native_stop_exception_node>());
    (void)implementation;
    return operation;
}

const hg::Operator &native_pair_consumer_operator() {
    static auto operation = hg::operator_<auto(hg::TSB<NativePairSchema>) -> hg::TS<hg::i64>>{
        "native_pair_consumer", {"value"}};
    static const auto &implementation = operation.overload(hg::node([](hg::In<hg::TSB<NativePairSchema>> input) {
        return *input->value;
    }));
    (void)implementation;
    return operation;
}

const hg::Operator &native_start_only_source_operator() {
    static auto operation = hg::operator_<auto() -> hg::TS<hg::i64>>{"native_start_only_source"};
    static const auto &implementation = operation.overload(
        hg::python::node(native_start_only_source_definition{}));
    (void)implementation;
    return operation;
}

const hg::Operator &native_continuous_timer_operator() {
    static auto operation = hg::operator_<auto(hg::TS<bool>) -> hg::TS<hg::i64>>{"native_continuous_timer",
                                                                                 {"repeat"}};
    static const auto &implementation = operation.overload(hg::node<native_continuous_timer_node>());
    (void)implementation;
    return operation;
}

const hg::Operator &native_python_object_property_operator() {
    static auto operation = hg::operator_<auto(hg::TS_T) -> hg::TS_T>{"native_python_object_property", {"value"}};
    static const auto &implementation = operation.overload(hg::python::node<native_python_object_property_node>());
    (void)implementation;
    return operation;
}

struct list_source_definition {
    hg::Out<hg::TSL<hg::TS<hg::i64>, 2>> output;

    void start() const {
        output[0].set(10);
        output[1].set(20);
    }

    void evaluate() const {}
};

const hg::Operator &discard_operator() {
    static auto operation = hg::operator_<void(hg::TS_T)>{"discard", {"value"}};
    static const auto &implementation = operation.overload(hg::node([](hg::In<hg::TS_T>) {}));
    (void)implementation;

    return operation;
}

const hg::Operator &invalidate_on_zero_operator() {
    static auto operation = hg::operator_<auto(hg::TS_T) -> hg::TS_T>{"invalidate_on_zero", {"value"}};
    static const auto &implementation = operation.overload(hg::node([](hg::In<hg::TS<hg::i64>> input,
                                                                       hg::Out<hg::TS<hg::i64>> output) {
        if (input.value() == 0) {
            output.invalidate();
            return;
        }

        output.set(input.value());
    }));
    (void)implementation;

    return operation;
}

const hg::Operator &optional_choice_operator() {
    static auto operation = hg::operator_<auto(hg::TS_T) -> hg::TS_T>{"optional_choice", {"value"}};
    static const auto &required = operation.overload(hg::node([](hg::In<hg::TS<hg::i64>> input,
                                                                 hg::Out<hg::TS<hg::i64>> output) {
        output.set(input.value() + 10);
    }));
    static const auto &optional = operation.overload(hg::node([](hg::In<hg::TS<hg::i64>, hg::optional> input,
                                                                 hg::Out<hg::TS<hg::i64>> output) {
        output.set((input.valid() ? input.value() : 0) + 20);
    }));
    (void)required;
    (void)optional;

    return operation;
}

nanobind::object runtime_double_to_python(hg::Scalar<>, const hg::python::PythonScalarConversions &) {
    return nanobind::str{"runtime double conversion"};
}

nanobind::object runtime_double_to_python_view(hg::Scalar<>, const hg::python::PythonScalarConversions &,
                                               const std::shared_ptr<hg::python::CallbackLifetime> &,
                                               std::uint64_t) {
    return nanobind::str{"runtime double view conversion"};
}

nanobind::object extension_scalar_to_python(hg::Scalar<>, const hg::python::PythonScalarConversions &) {
    return nanobind::str{"extension scalar conversion"};
}

} // namespace

NB_MODULE(_hg_test_extension, module) {
    module.def("export_native_pair_schema", [](nanobind::handle python_type) {
        hg::python::export_schema(python_type, native_pair_time_series_schema(), &native_pair_scalar_schema());
    });
    module.def("export_native_tsb_schema", [](nanobind::handle python_type) {
        hg::python::export_schema(python_type, native_pair_time_series_schema());
    });
    module.def("export_native_optional_schema", [](nanobind::handle python_type) {
        hg::python::export_schema(python_type, native_optional_time_series_schema(), &native_optional_scalar_schema());
    });
    module.def("export_native_optional_tsb_schema", [](nanobind::handle python_type) {
        hg::python::export_schema(python_type, native_optional_time_series_schema());
    });
    module.def("export_conflicting_tsb_schema", [](nanobind::handle python_type) {
        hg::python::export_schema(python_type, conflicting_native_time_series_schema());
    });
    module.def("export_native_service", [](nanobind::handle python_service) {
        hg::python::export_service(python_service, native_python_service);
    });
    module.def("export_alternate_native_service", [](nanobind::handle python_service) {
        hg::python::export_service(python_service, alternate_native_python_service);
    });
    hg::python::export_operator(module, "const_", hg::operators::const_);
    hg::python::export_operator(module, "discard", discard_operator());
    hg::python::export_operator(module, "invalidate_on_zero", invalidate_on_zero_operator());
    hg::python::export_operator(module, "optional_choice", optional_choice_operator());
    hg::python::export_operator(module, "native_gil", native_gil_operator());
    hg::python::export_operator(module, "native_callable_gil", native_callable_gil_operator());
    hg::python::export_operator(module, "native_python_exception", native_python_exception_operator());
    hg::python::export_operator(module, "native_stop_exception", native_stop_exception_operator());
    hg::python::export_operator(module, "native_pair_consumer", native_pair_consumer_operator());
    hg::python::export_operator(module, "native_start_only_source", native_start_only_source_operator());
    hg::python::export_operator(module, "native_continuous_timer", native_continuous_timer_operator());
    hg::python::export_operator(module, "native_python_object_property", native_python_object_property_operator());
    hg::python::export_operator(module, "native_concrete_type_name", native_concrete_type_name_operator());
    hg::python::register_wiring_method(hg::TS<hg::SCALAR>::type(), "plus", hg::operators::add);
    hg::python::register_wiring_method(hg::TS<hg::i64>::type(), "plus", hg::operators::add);
    hg::python::register_wiring_method(hg::TS<hg::SCALAR>::type(), "plus", hg::operators::add);
    hg::python::register_wiring_method(hg::TS<hg::SCALAR>::type(), "type_name", hg::operators::add);
    module.def("register_conflicting_wiring_method", [] {
        hg::python::register_wiring_method(hg::TS<hg::i64>::type(), "plus", hg::operators::sub);
    });
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
        static const auto source = hg::node<list_source_definition>();
        const auto point = source();
        return nanobind::cast(hg::python::PythonWiringPoint{point, hg::python::PythonWiringScope::current()});
    });
    module.def("register_double_type", [](nanobind::handle python_type) {
        return hg::python::scalar_conversions().add_python_type(python_type, hg::double_type());
    });
    module.def("register_runtime_double_result_conversion", [] {
        return hg::python::scalar_conversions().add(hg::cpp_scalar_runtime_type<double>(),
                                                    &extension_scalar_to_python);
    });
    module.def("can_convert_python_type", [](nanobind::handle python_type) {
        auto &conversions = hg::python::scalar_conversions();
        return conversions.can_from_python(conversions.type_from_python(python_type));
    });
    module.def("python_type_supports_value_covariance", [](nanobind::handle python_type) {
        return hg::python::scalar_conversions().type_from_python(python_type).supports_value_covariance();
    });
    module.def("copy_release_python_scalar_without_gil", [](nanobind::handle value) {
        auto memory = hg::allocator{};
        auto original = std::optional<hg::ScalarValue<>>{};
        original.emplace(hg::python::scalar_conversions().from_python(value, memory));
        {
            auto release_gil = nanobind::gil_scoped_release{};
            {
                auto copy = hg::ScalarValue<>{original->view()};
                original.reset();
            }
        }
        hg::python::Object::drain_deferred_releases();
        return true;
    });
    module.def("runtime_conversion_overrides_type_conversion", [] {
        auto conversions = hg::python::PythonScalarConversions{};
        const auto added = conversions.add(hg::cpp_scalar_runtime_type<double>(), &runtime_double_to_python);
        const auto view_added =
            conversions.add(hg::cpp_scalar_runtime_type<double>(), &runtime_double_to_python_view);
        const auto value = 42.0;
        const auto typed_scalar = hg::Scalar<double>::from(value);
        const auto scalar = hg::Scalar<>::from(typed_scalar);
        const auto lifetime = std::make_shared<hg::python::CallbackLifetime>();
        const auto callback = hg::python::CallbackScope{*lifetime};
        return nanobind::make_tuple(added, conversions.to_python(scalar), view_added,
                                    conversions.to_python_view(scalar, lifetime, callback.generation()));
    });
    module.def("date_time_conversions_can_be_registered", [] {
        auto conversions = hg::python::PythonScalarConversions{};
        const auto date_time_added = conversions.add(hg::evaluation_time_type(), &extension_scalar_to_python);
        const auto timedelta_added = conversions.add(hg::timedelta_type(), &extension_scalar_to_python);

        const auto date_time = hg::date_time_t{};
        const auto timedelta = hg::timedelta_t{};
        const auto typed_date_time = hg::Scalar<hg::date_time_t>::from(date_time);
        const auto typed_timedelta = hg::Scalar<hg::timedelta_t>::from(timedelta);
        const auto date_time_scalar = hg::Scalar<>::from(typed_date_time);
        const auto timedelta_scalar = hg::Scalar<>::from(typed_timedelta);
        const auto lifetime = std::make_shared<hg::python::CallbackLifetime>();
        const auto callback = hg::python::CallbackScope{*lifetime};
        return nanobind::make_tuple(date_time_added, conversions.to_python(date_time_scalar), timedelta_added,
                                    conversions.to_python(timedelta_scalar),
                                    conversions.to_python_view(date_time_scalar, lifetime, callback.generation()));
    });
    module.def("evaluation_time_microseconds", [](hg::EvaluationTime value) {
        return value.time_since_epoch().count();
    });
    module.def("evaluation_time_from_microseconds", [](std::int64_t value) {
        return hg::EvaluationTime{std::chrono::microseconds{value}};
    });
    module.def("convert_timedelta", [](nanobind::handle value) {
        auto memory = hg::allocator{};
        auto &conversions = hg::python::scalar_conversions();
        const auto converted = conversions.from_python(hg::timedelta_type(), value, memory);
        const auto view = converted.view();
        return conversions.to_python(hg::Scalar<>::from(view));
    });
    module.def("bytes_conversion_addresses", [](nanobind::handle value) {
        auto memory = hg::allocator{};
        auto &conversions = hg::python::scalar_conversions();

        Py_buffer input_buffer{};
        if (PyObject_GetBuffer(value.ptr(), &input_buffer, PyBUF_CONTIG_RO) < 0) {
            nanobind::raise_python_error();
        }
        auto release_input = hg::util::scope_exit{[&]() noexcept { PyBuffer_Release(&input_buffer); }};

        const auto converted = conversions.from_python(hg::bytes_type(), value, memory);
        const auto native = hg::unchecked_scalar_cast<hg::bytes>(converted).value();
        const auto input_shares = input_buffer.buf == native.data() &&
                                  static_cast<std::size_t>(input_buffer.len) == native.size();

        auto exported = conversions.to_python(converted.view());
        Py_buffer output_buffer{};
        if (PyObject_GetBuffer(exported.ptr(), &output_buffer, PyBUF_CONTIG_RO) < 0) {
            nanobind::raise_python_error();
        }
        auto release_output = hg::util::scope_exit{[&]() noexcept { PyBuffer_Release(&output_buffer); }};
        const auto export_shares = output_buffer.buf == native.data() &&
                                   static_cast<std::size_t>(output_buffer.len) == native.size();

        const auto round_trip = conversions.from_python(hg::bytes_type(), exported, memory);
        const auto round_trip_native = hg::unchecked_scalar_cast<hg::bytes>(round_trip).value();
        const auto import_shares = round_trip_native.data() == native.data() &&
                                   round_trip_native.size() == native.size();
        return nanobind::make_tuple(input_shares, export_shares, import_shares);
    });
    module.def("round_trip_bytes", [](nanobind::handle value) {
        auto memory = hg::allocator{};
        auto &conversions = hg::python::scalar_conversions();
        const auto converted = conversions.from_python(hg::bytes_type(), value, memory);
        return conversions.to_python(converted.view());
    });
    module.def("scalar_conversions_address",
               [] { return reinterpret_cast<std::uintptr_t>(&hg::python::scalar_conversions()); });
    module.def("decode_invalid_string", [] {
        const auto invalid_utf8 = std::string_view{"\xFF", 1};
        const auto value = hg::ScalarValue<hg::string>{invalid_utf8};
        return hg::python::scalar_conversions().to_python(hg::Scalar<>::from(value));
    });
}

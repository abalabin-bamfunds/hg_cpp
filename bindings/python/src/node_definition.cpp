#include <bg/python/node_definition.hpp>

#include <bg/graph/runtime_graph.hpp>
#include <bg/node/output.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <array>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg::python {

using Invocation = NodeInvocation<PythonNodeDefinition::compiled_data_type>;
namespace {

class CallArguments {
  public:
    explicit CallArguments(std::size_t size)
        : overflow_(size > inline_capacity ? size : 0),
          values_{overflow_.empty() ? std::span<PyObject *>{inline_.data(), size}
                                    : std::span<PyObject *>{overflow_}} {}

    ~CallArguments() {
        for (auto *value : values_) {
            Py_XDECREF(value);
        }
    }

    CallArguments(const CallArguments &) = delete;
    CallArguments &operator=(const CallArguments &) = delete;

    void set(std::size_t index, nanobind::object value) noexcept { values_[index] = value.release().ptr(); }
    PyObject *const *data() const noexcept { return values_.data(); }
    std::size_t size() const noexcept { return values_.size(); }

  private:
    static constexpr auto inline_capacity = std::size_t{8};

    std::array<PyObject *, inline_capacity> inline_{};
    std::vector<PyObject *> overflow_;
    std::span<PyObject *> values_;
};

} // namespace

nanobind::object PythonNodeDefinition::CompiledData::argument(Invocation invocation, std::size_t index,
                                                              CallbackScope &callback) const {
    const auto &parameter = parameters[index];
    if (parameter.time_series) {
        auto input = invocation.input<In<TS<SCALAR>>>(index, parameter.runtime_type_index);
        return nanobind::cast(InputView{input, scalar_conversions(), lifetime, callback.generation()});
    }

    const auto scalar = invocation.scalar(index, parameter.runtime_type_index);
    if (!scalar.has_value()) {
        return nanobind::none();
    }
    return scalar_conversions().to_python(scalar.value());
}

nanobind::object PythonNodeDefinition::CompiledData::call(Invocation invocation, CallbackScope &callback) const {
    auto arguments = CallArguments{parameters.size()};
    for (auto index = std::size_t{}; index < parameters.size(); ++index) {
        arguments.set(index, argument(invocation, index, callback));
    }

    auto *result = PyObject_Vectorcall(function.get().ptr(), arguments.data(), arguments.size(), nullptr);
    if (result == nullptr) {
        nanobind::raise_python_error();
    }
    return nanobind::steal<nanobind::object>(result);
}

void PythonNodeDefinition::CompiledData::apply_delta(Invocation invocation, nanobind::handle delta) const {
    if (delta.is_none()) {
        return;
    }
    if (output_scalar_type == nullptr) {
        throw std::invalid_argument{"a Python sink node must return None"};
    }

    auto &memory = invocation.context().runtime_graph().allocation();
    auto value = output_conversion(delta, memory, scalar_conversions());
    invocation.output<TS<SCALAR>>().set_value(value.view());
}

void PythonNodeDefinition::CompiledData::invoke(Invocation invocation) const {
    if (!Py_IsInitialized()) {
        throw std::runtime_error{"the Python interpreter has shut down"};
    }
    auto gil = nanobind::gil_scoped_acquire{};

    auto callback = CallbackScope{*lifetime};
    auto result = call(invocation, callback);
    apply_delta(invocation, result);
}

PythonNodeDefinition::PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature,
                                           nanobind::handle function, bool schedule_on_start)
    : signature_{std::move(signature)}, weak_function_{function}, identity_{std::make_shared<const char>()},
      schedule_on_start_{schedule_on_start} {
    if (signature_ == nullptr) {
        throw std::invalid_argument{"a Python node requires a signature"};
    }
    if (!PyCallable_Check(function.ptr())) {
        throw std::invalid_argument{"a Python node definition requires a callable"};
    }
}

PythonNodeDefinition::PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature, Object function,
                                           std::shared_ptr<const char> identity, bool schedule_on_start) noexcept
    : signature_{std::move(signature)}, retained_function_{std::move(function)}, identity_{std::move(identity)},
      schedule_on_start_{schedule_on_start} {}

PythonNodeDefinition PythonNodeDefinition::retain_function() const {
    return PythonNodeDefinition{signature_, weak_function_.lock(), identity_, schedule_on_start_};
}

NodeTemplateIdentity PythonNodeDefinition::definition_identity() const {
    return NodeTemplateIdentity::from_value(Identity{identity_});
}

PythonNodeDefinition::CompiledData PythonNodeDefinition::compile(const ResolvedSignature &signature,
                                                                 allocator &) const {
    if (!retained_function_) {
        throw std::logic_error{"a Python node callback was not retained by its graph wiring"};
    }

    auto time_series_runtime_type_index = std::size_t{};
    auto scalar_runtime_type_index = std::size_t{};
    auto parameter_conversions = std::vector<ParameterConversion>{};
    parameter_conversions.reserve(signature.parameters.size());

    for (const auto &parameter : signature.parameters) {
        const auto time_series = parameter.type->is_time_series();

        if (time_series) {
            const auto *value_type = type_cast<TimeSeriesValueType>(parameter.type);
            if (value_type == nullptr) {
                throw std::invalid_argument{"Python node inputs currently require TS<T> values"};
            }
        } else {
            if (type_cast<ScalarType>(parameter.type) == nullptr) {
                throw std::invalid_argument{"Python node inputs must be scalar values or TS<T> values"};
            }
        }

        auto &runtime_type_index = time_series ? time_series_runtime_type_index : scalar_runtime_type_index;
        parameter_conversions.push_back({time_series, runtime_type_index++});
    }

    const ScalarType *output_scalar_type = nullptr;
    if (signature.output != nullptr) {
        const auto *output_type = type_cast<TimeSeriesValueType>(signature.output);
        if (output_type == nullptr) {
            throw std::invalid_argument{"Python node outputs currently require TS<T>"};
        }
        output_scalar_type = &output_type->value_type();
    }

    auto output_conversion = PythonToScalar{};
    if (output_scalar_type != nullptr) {
        output_conversion = scalar_conversions().from_python_handler(*output_scalar_type);
        if (output_conversion == nullptr) {
            throw std::invalid_argument{"no Python conversion is registered for a Python node output"};
        }
    }

    auto result = CompiledData{};
    result.function = retained_function_;
    result.parameters = std::move(parameter_conversions);
    result.output_scalar_type = output_scalar_type;
    result.output_conversion = output_conversion;
    return result;
}

void PythonNodeDefinition::evaluate(Invocation invocation) {
    invocation.compiled_data().invoke(invocation);
}

} // namespace bg::python

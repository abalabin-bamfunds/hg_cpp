#include <bg/python/node_definition.hpp>

#include "delta.hpp"

#include <bg/graph/runtime_graph.hpp>
#include <bg/node/output.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>
#include <bg/time_series/tss/tss_output.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <nanobind/stl/string.h>

#include <array>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg::python {

using Invocation = NodeInvocation<PythonNodeDefinition::compiled_data_type>;
namespace {

void apply_time_series_delta(Out<TS_T> output, nanobind::handle delta, allocator &memory);

void apply_value_delta(Out<TS<SCALAR>> &output, const TimeSeriesRuntimeType &runtime_type, nanobind::handle delta,
                       allocator &memory) {
    const auto &type = type_cast<TimeSeriesValueType>(runtime_type.type());
    auto value = scalar_conversions().from_python(type.value_type(), delta, memory);
    output.set_value(value.view());
}

void apply_set_delta(Out<TSS<SCALAR>> &output, const TimeSeriesRuntimeType &runtime_type, nanobind::handle delta,
                     allocator &memory) {
    if (!details::is_set_delta(delta)) {
        throw std::invalid_argument{"a TSS output requires SetDelta"};
    }

    const auto &type = type_cast<TimeSeriesSetType>(runtime_type.type());
    const auto apply = [&](nanobind::handle values, auto &&function) {
        for (const auto value : nanobind::borrow<nanobind::iterable>(values)) {
            auto scalar = scalar_conversions().from_python(type.item_type(), value, memory);
            function(Scalar<SCALAR>::from(scalar));
        }
    };

    apply(delta.attr("added"), [&](const Scalar<SCALAR> &value) { output.insert(value); });
    apply(delta.attr("removed"), [&](const Scalar<SCALAR> &value) { output.erase(value); });
}

void apply_list_delta(Out<TSL<TS_T, SIZE>> &output, nanobind::handle delta, allocator &memory) {
    if (PyMapping_Check(delta.ptr()) == 0) {
        throw std::invalid_argument{"a TSL output delta must be a mapping of indices to item deltas"};
    }

    auto items = nanobind::steal<nanobind::object>(PyMapping_Items(delta.ptr()));
    if (!items) {
        nanobind::raise_python_error();
    }
    for (const auto entry : nanobind::borrow<nanobind::iterable>(items)) {
        const auto pair = nanobind::cast<nanobind::tuple>(entry);
        const auto index = nanobind::cast<std::size_t>(pair[0]);
        if (index >= output.size()) {
            throw std::out_of_range{"a TSL output delta index is out of range"};
        }
        auto item = output[index];
        apply_time_series_delta(std::move(item), pair[1], memory);
    }
}

void apply_bundle_delta(Out<TSB<SCHEMA>> &output, const TimeSeriesRuntimeType &runtime_type, nanobind::handle delta,
                        allocator &memory) {
    if (PyMapping_Check(delta.ptr()) == 0) {
        throw std::invalid_argument{"a TSB output delta must be a mapping of field names to item deltas"};
    }

    const auto &type = type_cast<TimeSeriesBundleType>(runtime_type.type());
    auto items = nanobind::steal<nanobind::object>(PyMapping_Items(delta.ptr()));
    if (!items) {
        nanobind::raise_python_error();
    }
    for (const auto entry : nanobind::borrow<nanobind::iterable>(items)) {
        const auto pair = nanobind::cast<nanobind::tuple>(entry);
        const auto name_text = nanobind::cast<std::string>(pair[0]);
        const auto name = util::symbol{std::string_view{name_text}};
        const auto index = type.find_item_index(name);
        if (!index) {
            throw std::out_of_range{"a TSB output delta names an unknown field"};
        }
        auto item = output[*index];
        apply_time_series_delta(std::move(item), pair[1], memory);
    }
}

void apply_dictionary_items(Out<TSD<SCALAR, TS_T>> &output, nanobind::handle values, const ScalarType &key_type,
                            allocator &memory) {
    auto items = nanobind::steal<nanobind::object>(PyMapping_Items(values.ptr()));
    if (!items) {
        nanobind::raise_python_error();
    }
    for (const auto entry : nanobind::borrow<nanobind::iterable>(items)) {
        const auto pair = nanobind::cast<nanobind::tuple>(entry);
        auto key = scalar_conversions().from_python(key_type, pair[0], memory);
        auto typed_key = Scalar<SCALAR>::from(key);
        if (details::is_remove(pair[1])) {
            output.erase(typed_key);
            continue;
        }

        auto item = output[typed_key];
        apply_time_series_delta(std::move(item), pair[1], memory);
    }
}

void apply_dictionary_delta(Out<TSD<SCALAR, TS_T>> &output, const TimeSeriesRuntimeType &runtime_type,
                            nanobind::handle delta, allocator &memory) {
    const auto &type = type_cast<TimeSeriesDictionaryType>(runtime_type.type());
    if (details::is_tsd_delta(delta)) {
        const auto removed = delta.attr("removed");
        for (const auto value : nanobind::borrow<nanobind::iterable>(removed)) {
            auto key = scalar_conversions().from_python(type.key_type(), value, memory);
            output.erase(Scalar<SCALAR>::from(key));
        }
        const auto modified = delta.attr("modified");
        apply_dictionary_items(output, modified, type.key_type(), memory);
        return;
    }

    if (PyMapping_Check(delta.ptr()) == 0) {
        throw std::invalid_argument{"a TSD output delta must be a mapping or TSDDelta"};
    }
    apply_dictionary_items(output, delta, type.key_type(), memory);
}

void apply_reference_delta(Out<REF<TS_T>> &output, nanobind::handle delta) {
    output.set_value(nanobind::cast<ReferenceValue>(delta).value());
}

void apply_time_series_delta(Out<TS_T> output, nanobind::handle delta, allocator &memory) {
    if (delta.is_none()) {
        return;
    }

    const auto &runtime_type = output.runtime_type();
    output.visit(
        [&](Out<TS<SCALAR>> *value) { apply_value_delta(*value, runtime_type, delta, memory); },
        [&](Out<TSS<SCALAR>> *value) { apply_set_delta(*value, runtime_type, delta, memory); },
        [&](Out<TSL<TS_T, SIZE>> *value) { apply_list_delta(*value, delta, memory); },
        [&](Out<TSB<SCHEMA>> *value) { apply_bundle_delta(*value, runtime_type, delta, memory); },
        [&](Out<TSD<SCALAR, TS_T>> *value) { apply_dictionary_delta(*value, runtime_type, delta, memory); },
        [&](Out<REF<TS_T>> *value) { apply_reference_delta(*value, delta); }, ddv::noop);
}

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
        auto input = invocation.input<In<TS_T>>(index, parameter.runtime_type_index);
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
    if (!has_output) {
        throw std::invalid_argument{"a Python sink node must return None"};
    }

    auto &memory = invocation.context().runtime_graph().allocation();
    apply_time_series_delta(invocation.output(), delta, memory);
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

        if (!time_series) {
            if (type_cast<ScalarType>(parameter.type) == nullptr) {
                throw std::invalid_argument{"Python node inputs must be scalar or time-series values"};
            }
        }

        auto &runtime_type_index = time_series ? time_series_runtime_type_index : scalar_runtime_type_index;
        parameter_conversions.push_back({time_series, runtime_type_index++});
    }

    auto result = CompiledData{};
    result.function = retained_function_;
    result.parameters = std::move(parameter_conversions);
    result.has_output = signature.output != nullptr;
    return result;
}

void PythonNodeDefinition::evaluate(Invocation invocation) {
    invocation.compiled_data().invoke(invocation);
}

} // namespace bg::python

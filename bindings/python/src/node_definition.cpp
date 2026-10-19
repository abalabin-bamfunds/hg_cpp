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
#include <bg/util/scope_exit.hpp>

#include <nanobind/stl/string.h>
#include <nanobind/stl/chrono.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bg::python {

using Invocation = NodeInvocation<PythonNodeDefinition::compiled_data_type, PythonNodeDefinition::state_type>;

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
    const auto set_delta = details::is_set_delta(delta);
    if (!set_delta && PySet_Check(delta.ptr()) == 0 && PyFrozenSet_Check(delta.ptr()) == 0) {
        throw std::invalid_argument{"a TSS output requires a set, frozenset, or SetDelta"};
    }

    const auto &type = type_cast<TimeSeriesSetType>(runtime_type.type());
    const auto apply_values = [&](nanobind::handle values, auto &&apply) {
        for (const auto value : nanobind::borrow<nanobind::iterable>(values)) {
            auto scalar = scalar_conversions().from_python(type.item_type(), value, memory);
            apply(Scalar<SCALAR>::from(scalar));
        }
    };

    if (set_delta) {
        apply_values(delta.attr("added"), [&](const Scalar<SCALAR> &value) { output.insert(value); });
        apply_values(delta.attr("removed"), [&](const Scalar<SCALAR> &value) { output.erase(value); });
        return;
    }

    for (const auto value : nanobind::borrow<nanobind::iterable>(delta)) {
        const auto removed = details::is_removed(value);
        const auto item = removed ? value.attr("item") : value;
        auto scalar = scalar_conversions().from_python(type.item_type(), item, memory);
        if (removed) {
            output.erase(Scalar<SCALAR>::from(scalar));
        } else {
            output.insert(Scalar<SCALAR>::from(scalar));
        }
    }
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
    if (PyMapping_Check(delta.ptr()) == 0) {
        throw std::invalid_argument{"a TSD output delta must be a mapping"};
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
                                                              CallbackScope &callback, bool retain_scalar) const {
    const auto &parameter = parameters[index];
    if (parameter.mode == ParameterMode::input_view) {
        auto input = invocation.input<In<TS_T>>(index);
        return make_input_view(input, scalar_conversions(), lifetime, callback.generation());
    }

    if (parameter.mode != ParameterMode::scalar) {
        auto input = invocation.input<In<TS<SCALAR>>>(index);
        if (!input.valid()) {
            return nanobind::none();
        }

        const auto value = input.value_view();
        return scalar_conversions().to_python_view(Scalar<>::from(value), lifetime, callback.generation());
    }

    const auto scalar = invocation.scalar(index);
    if (!scalar.has_value()) {
        return nanobind::none();
    }

    if (retain_scalar) {
        return scalar_conversions().to_python(scalar.value());
    }
    return scalar_conversions().to_python_view(scalar.value(), lifetime, callback.generation());
}

nanobind::object PythonNodeDefinition::CompiledData::call(
    Invocation invocation, CallbackScope &callback, const Object &callback_function,
    const CallbackArguments &callback_arguments, bool generator_call) const {
    auto arguments = CallArguments{callback_arguments.size()};
    for (auto argument_index = std::size_t{}; argument_index < callback_arguments.size(); ++argument_index) {
        const auto parameter = callback_arguments[argument_index];
        switch (parameter.parameter) {
        case CallbackParameter::input:
            arguments.set(argument_index, argument(invocation, parameter.index, callback, generator_call));
            break;
        case CallbackParameter::output: {
            auto output = invocation.output();
            const auto modified = output.valid() && output.last_updated() == invocation.context().evaluation_time();
            auto input = In<TS_T>{output.output_state().mutable_last_updated(), output.data(), output.runtime_type(),
                                  modified};
            arguments.set(argument_index,
                          make_input_view(input, scalar_conversions(), lifetime, callback.generation()));
            break;
        }
        case CallbackParameter::evaluation_clock:
            arguments.set(argument_index, nanobind::cast(PythonEvaluationClock{NodeCallbackAccess{
                invocation.context(), lifetime, callback.generation(),
                generator_call ? runtime_state(invocation).generator_clock : nullptr}}));
            break;
        case CallbackParameter::scheduler:
            arguments.set(argument_index, nanobind::cast(PythonScheduler{
                NodeCallbackAccess{invocation.context(), lifetime, callback.generation()}}));
            break;
        case CallbackParameter::state: {
            auto &storage = invocation.state<void *>();
            auto *state = is_generator ? runtime_state(invocation).user_state : static_cast<PyObject *>(storage);
            if (state == nullptr && state_factory) {
                state = PyObject_CallNoArgs(state_factory.get().ptr());
                if (state == nullptr) {
                    nanobind::raise_python_error();
                }
                if (is_generator) {
                    runtime_state(invocation).user_state = state;
                } else {
                    storage = state;
                }
            }
            auto value = state == nullptr ? nanobind::none() : nanobind::borrow<nanobind::object>(state);
            arguments.set(argument_index, std::move(value));
            break;
        }
        }
    }

    auto *result = PyObject_Vectorcall(callback_function.get().ptr(), arguments.data(), arguments.size(), nullptr);
    if (result == nullptr) {
        nanobind::raise_python_error();
    }
    return nanobind::steal<nanobind::object>(result);
}

PythonNodeDefinition::RuntimeState &PythonNodeDefinition::CompiledData::runtime_state(Invocation invocation) const {
    auto &storage = invocation.state<void *>();
    if (storage == nullptr) {
        storage = new RuntimeState{};
    }
    return *static_cast<RuntimeState *>(storage);
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

void PythonNodeDefinition::CompiledData::start(Invocation invocation) const {
    if (!Py_IsInitialized()) {
        throw std::runtime_error{"the Python interpreter has shut down"};
    }
    auto gil = nanobind::gil_scoped_acquire{};
    auto callback = CallbackScope{*lifetime};

    if (is_generator) {
        auto generator_value = call(invocation, callback, function, evaluation_arguments, true);
        auto iterator = nanobind::steal<nanobind::object>(PyObject_GetIter(generator_value.ptr()));
        if (!iterator) {
            nanobind::raise_python_error();
        }
        runtime_state(invocation).generator = iterator.release().ptr();
        advance_generator(invocation);
        return;
    }

    if (start_function) {
        (void)call(invocation, callback, start_function, start_arguments);
    }
}

void PythonNodeDefinition::CompiledData::evaluate(Invocation invocation) const {
    if (!Py_IsInitialized()) {
        throw std::runtime_error{"the Python interpreter has shut down"};
    }
    auto gil = nanobind::gil_scoped_acquire{};
    auto callback = CallbackScope{*lifetime};

    if (is_generator) {
        auto &state = runtime_state(invocation);
        if (state.pending_value != nullptr) {
            auto pending = nanobind::steal<nanobind::object>(std::exchange(state.pending_value, nullptr));
            apply_delta(invocation, pending);
        }
        advance_generator(invocation);
        return;
    }

    if (function) {
        auto result = call(invocation, callback, function, evaluation_arguments);
        apply_delta(invocation, result);
    }
}

void PythonNodeDefinition::CompiledData::stop(Invocation invocation) const noexcept {
    const auto context = stop_function ? stop_function.get() : function.get();
    const auto report_unraisable = [](std::exception_ptr failure, nanobind::handle error_context) noexcept {
        try {
            std::rethrow_exception(failure);
        } catch (nanobind::python_error &error) {
            error.discard_as_unraisable(error_context);
        } catch (const std::exception &error) {
            PyErr_SetString(PyExc_RuntimeError, error.what());
            PyErr_WriteUnraisable(error_context.ptr());
        } catch (...) {
            PyErr_SetString(PyExc_RuntimeError, "an unknown exception occurred in a Python node stop callback");
            PyErr_WriteUnraisable(error_context.ptr());
        }
    };

    try {
        if (!Py_IsInitialized()) {
            return;
        }
        auto gil = nanobind::gil_scoped_acquire{};
        auto *generator_state = is_generator ? &runtime_state(invocation) : nullptr;
        if (generator_state != nullptr) {
            generator_state->generator_clock->current = &invocation.context();
        }
        auto clear_generator_context = util::scope_exit{[generator_state]() noexcept {
            if (generator_state != nullptr) {
                generator_state->generator_clock->current = nullptr;
            }
        }};

        auto failures = std::array<std::exception_ptr, 2>{};
        auto failure_count = std::size_t{};
        if (stop_function) {
            try {
                auto callback = CallbackScope{*lifetime};
                (void)call(invocation, callback, stop_function, stop_arguments);
            } catch (...) {
                failures[failure_count++] = std::current_exception();
            }
        }

        if (is_generator) {
            try {
                auto &state = *generator_state;
                if (state.generator != nullptr) {
                    auto generator = nanobind::borrow<nanobind::object>(state.generator);
                    auto closed = nanobind::steal<nanobind::object>(
                        PyObject_CallMethod(generator.ptr(), "close", nullptr));
                    if (!closed) {
                        nanobind::raise_python_error();
                    }
                }
            } catch (...) {
                failures[failure_count++] = std::current_exception();
            }
        }
        for (auto index = std::size_t{}; index < failure_count; ++index) {
            report_unraisable(std::move(failures[index]), context ? context : nanobind::handle{Py_None});
        }
    } catch (...) {
        if (!Py_IsInitialized()) {
            return;
        }
        try {
            auto gil = nanobind::gil_scoped_acquire{};
            report_unraisable(std::current_exception(), context ? context : nanobind::handle{Py_None});
        } catch (...) {
            // Interpreter shutdown can prevent reporting from the noexcept stop callback.
        }
    }
}

void PythonNodeDefinition::CompiledData::on_invalidation(Invocation invocation) const {
    if (!invalidation_function) {
        return;
    }
    if (!Py_IsInitialized()) {
        throw std::runtime_error{"the Python interpreter has shut down"};
    }
    auto gil = nanobind::gil_scoped_acquire{};
    auto callback = CallbackScope{*lifetime};
    auto result = call(invocation, callback, invalidation_function, invalidation_arguments);
    apply_delta(invocation, result);
}

void PythonNodeDefinition::CompiledData::advance_generator(Invocation invocation) const {
    auto &state = runtime_state(invocation);
    auto iterator = nanobind::borrow<nanobind::object>(state.generator);
    state.generator_clock->current = &invocation.context();
    auto clear_generator_context = util::scope_exit{[&]() noexcept { state.generator_clock->current = nullptr; }};
    auto yielded = nanobind::steal<nanobind::object>(PyIter_Next(iterator.ptr()));
    state.generator_clock->current = nullptr;
    clear_generator_context.release();
    if (!yielded) {
        if (PyErr_Occurred() != nullptr) {
            nanobind::raise_python_error();
        }
        return;
    }

    auto pair = nanobind::cast<nanobind::tuple>(yielded);
    if (pair.size() != 2) {
        throw std::invalid_argument{"a Python generator must yield (datetime | timedelta, value) pairs"};
    }

    auto when = EvaluationTime{};
    const auto time = pair[0];
    if (!nanobind::try_cast(time, when)) {
        auto delay = std::chrono::microseconds{};
        if (!nanobind::try_cast(time, delay)) {
            throw std::invalid_argument{"a Python generator time must be datetime or timedelta"};
        }
        when = invocation.context().evaluation_time() + delay;
    }

    const auto current_time = invocation.context().evaluation_time();
    if (state.has_generator_time && when <= state.last_generator_time) {
        throw std::invalid_argument{"Python generator output times must be strictly increasing"};
    }
    if (when < current_time) {
        throw std::invalid_argument{"a Python generator cannot yield a time before the current evaluation time"};
    }

    state.last_generator_time = when;
    state.has_generator_time = true;
    if (when == current_time) {
        apply_delta(invocation, pair[1]);
        advance_generator(invocation);
        return;
    }

    Py_XDECREF(state.pending_value);
    Py_INCREF(pair[1].ptr());
    state.pending_value = pair[1].ptr();
    invocation.context().schedule_at(when);
}

PythonNodeDefinition::PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature,
                                           bool schedule_on_start, bool pass_time_series_values,
                                           CallbackDefinition evaluation, CallbackDefinition start,
                                           CallbackDefinition stop, CallbackDefinition invalidation,
                                           nanobind::handle state_factory, bool generator)
    : signature_{std::move(signature)}, weak_function_{weak_callback(evaluation)},
      weak_start_function_{weak_callback(start)}, weak_stop_function_{weak_callback(stop)},
      weak_invalidation_function_{weak_callback(invalidation)}, identity_{std::make_shared<const char>()},
      schedule_on_start_{schedule_on_start}, pass_time_series_values_{pass_time_series_values},
      generator_{generator} {
    if (signature_ == nullptr) {
        throw std::invalid_argument{"a Python node requires a signature"};
    }
    for (const auto *callback : {&evaluation, &start, &stop, &invalidation}) {
        if (callback->function.ptr() != nullptr && !PyCallable_Check(callback->function.ptr())) {
            throw std::invalid_argument{"a Python node callback must be callable"};
        }
    }
    evaluation_arguments_ = std::move(evaluation.arguments);
    start_arguments_ = std::move(start.arguments);
    stop_arguments_ = std::move(stop.arguments);
    invalidation_arguments_ = std::move(invalidation.arguments);
    if (!state_factory.is_none()) {
        if (!PyCallable_Check(state_factory.ptr())) {
            throw std::invalid_argument{"a Python node State factory must be callable"};
        }
        state_factory_ = Object{state_factory};
    }
}

std::optional<WeakObject> PythonNodeDefinition::weak_callback(const CallbackDefinition &callback) {
    if (callback.function.ptr() == nullptr) {
        return std::nullopt;
    }
    return WeakObject{callback.function};
}

Object PythonNodeDefinition::retain_callback(const std::optional<WeakObject> &callback) {
    return callback ? callback->lock() : Object{};
}

PythonNodeDefinition::PythonNodeDefinition(std::shared_ptr<const SignatureDefinition> signature,
                                           std::shared_ptr<const char> identity, bool schedule_on_start,
                                           Object function, CallbackArguments evaluation_arguments,
                                           bool pass_time_series_values, Object start_function,
                                           CallbackArguments start_arguments, Object stop_function,
                                           CallbackArguments stop_arguments, Object invalidation_function,
                                           CallbackArguments invalidation_arguments, Object state_factory,
                                           bool generator) noexcept
    : signature_{std::move(signature)}, retained_function_{std::move(function)},
      evaluation_arguments_{std::move(evaluation_arguments)},
      retained_start_function_{std::move(start_function)}, start_arguments_{std::move(start_arguments)},
      retained_stop_function_{std::move(stop_function)}, stop_arguments_{std::move(stop_arguments)},
      retained_invalidation_function_{std::move(invalidation_function)},
      invalidation_arguments_{std::move(invalidation_arguments)}, state_factory_{std::move(state_factory)},
      identity_{std::move(identity)}, schedule_on_start_{schedule_on_start},
      pass_time_series_values_{pass_time_series_values}, generator_{generator} {}

PythonNodeDefinition PythonNodeDefinition::retain_function() const {
    return PythonNodeDefinition{signature_, identity_, schedule_on_start_, retain_callback(weak_function_),
                                evaluation_arguments_, pass_time_series_values_,
                                retain_callback(weak_start_function_), start_arguments_,
                                retain_callback(weak_stop_function_), stop_arguments_,
                                retain_callback(weak_invalidation_function_), invalidation_arguments_,
                                state_factory_, generator_};
}

bool PythonNodeDefinition::deduplicate_wiring() const noexcept {
    if (generator_) {
        return false;
    }
    const auto uses_state = [](const CallbackArguments &arguments) {
        return std::ranges::any_of(arguments, [](const auto &argument) {
            return argument.parameter == CallbackParameter::state;
        });
    };
    return !uses_state(evaluation_arguments_) && !uses_state(start_arguments_) && !uses_state(stop_arguments_) &&
           !uses_state(invalidation_arguments_);
}

bool PythonNodeDefinition::uses_timed_scheduling() const noexcept {
    const auto uses_scheduler = [](const CallbackArguments &arguments) {
        return std::ranges::any_of(arguments, [](const auto &argument) {
            return argument.parameter == CallbackParameter::scheduler;
        });
    };
    return generator_ || uses_scheduler(evaluation_arguments_) || uses_scheduler(start_arguments_) ||
           uses_scheduler(stop_arguments_) || uses_scheduler(invalidation_arguments_);
}

bool PythonNodeDefinition::schedule_on_start(const ResolvedSignature &signature) const noexcept {
    return schedule_on_start_ && std::ranges::none_of(signature.parameters, [](const auto &parameter) {
               return parameter.type->is_time_series();
           });
}

bool PythonNodeDefinition::has_evaluation(const ResolvedSignature &) const noexcept {
    return generator_ || static_cast<bool>(weak_function_) || static_cast<bool>(retained_function_);
}

bool PythonNodeDefinition::has_start(const ResolvedSignature &) const noexcept {
    return generator_ || weak_start_function_.has_value() || static_cast<bool>(retained_start_function_);
}

bool PythonNodeDefinition::has_stop(const ResolvedSignature &) const noexcept {
    return generator_ || weak_stop_function_.has_value() || static_cast<bool>(retained_stop_function_);
}

bool PythonNodeDefinition::has_on_invalidation(const ResolvedSignature &) const noexcept {
    return weak_invalidation_function_.has_value() || static_cast<bool>(retained_invalidation_function_);
}

NodeTemplateIdentity PythonNodeDefinition::definition_identity() const {
    return NodeTemplateIdentity::from_value(Identity{identity_});
}

PythonNodeDefinition::CompiledData PythonNodeDefinition::compile(const ResolvedSignature &signature,
                                                                 allocator &) const {
    if (!retained_function_ && !retained_start_function_) {
        throw std::logic_error{"a Python node callback was not retained by its graph wiring"};
    }

    auto parameter_conversions = std::vector<ParameterConversion>{};
    parameter_conversions.reserve(signature.parameters.size());

    for (std::size_t index = 0; index < signature.parameters.size(); ++index) {
        const auto &parameter = signature.parameters[index];
        const auto time_series = parameter.type->is_time_series();

        if (!time_series) {
            if (type_cast<ScalarType>(parameter.type) == nullptr) {
                throw std::invalid_argument{"Python node inputs must be scalar or time-series values"};
            }
        }

        auto mode = time_series ? ParameterMode::input_view : ParameterMode::scalar;
        if (time_series && pass_time_series_values_) {
            mode = ParameterMode::time_series_value;
        }

        parameter_conversions.push_back({mode});
    }

    auto result = CompiledData{};
    result.function = retained_function_;
    result.start_function = retained_start_function_;
    result.stop_function = retained_stop_function_;
    result.invalidation_function = retained_invalidation_function_;
    result.state_factory = state_factory_;
    result.parameters = std::move(parameter_conversions);
    result.evaluation_arguments = evaluation_arguments_;
    result.start_arguments = start_arguments_;
    result.stop_arguments = stop_arguments_;
    result.invalidation_arguments = invalidation_arguments_;
    result.has_output = signature.output != nullptr;
    result.is_generator = generator_;
    return result;
}

void PythonNodeDefinition::evaluate(Invocation invocation) {
    invocation.compiled_data().evaluate(invocation);
}

void PythonNodeDefinition::start(Invocation invocation) {
    invocation.compiled_data().start(invocation);
}

void PythonNodeDefinition::stop(Invocation invocation) noexcept {
    invocation.compiled_data().stop(invocation);
}

void PythonNodeDefinition::on_invalidation(Invocation invocation) {
    invocation.compiled_data().on_invalidation(invocation);
}

void PythonNodeDefinition::destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
    auto &storage = invocation.state<void *>();
    if (storage == nullptr) {
        return;
    }
    if (invocation.compiled_data().is_generator) {
        auto &state = *static_cast<RuntimeState *>(storage);
        if (state.user_state != nullptr) {
            Object::release_reference(state.user_state);
        }
        if (state.generator != nullptr) {
            Object::release_reference(state.generator);
        }
        if (state.pending_value != nullptr) {
            Object::release_reference(state.pending_value);
        }
        delete static_cast<RuntimeState *>(std::exchange(storage, nullptr));
        return;
    }
    Object::release_reference(static_cast<PyObject *>(std::exchange(storage, nullptr)));
}

} // namespace bg::python

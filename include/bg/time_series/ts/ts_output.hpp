#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <utility>

namespace bg {

template <details::scalar_variable Variable, auto... Properties> class Out<TS<Variable, Properties...>> {
  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)) {
        state_->bind(last_updated, value, context);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        value.input_view().assign(
            state_->mutable_data(),
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type());
        state_->publish();
    }

    void invalidate() const { state_->invalidate(); }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<Variable, Properties...>;

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

template <typename T, auto... Properties> class Out<TS<T, Properties...>> {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TS<T, Properties...>>()}, context)) {
        state_->bind(last_updated, &value, context);
    }

    Out(EvaluationTime &last_updated, storage_type &value, void *output_path)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TS<T, Properties...>>()}, output_path)) {
        state_->bind(last_updated, &value, output_path);
    }

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)) {
        state_->bind(last_updated, value, output_path);
    }

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    value_type value() const noexcept
        requires(!details::scalar_type_traits<T>::is_collection)
    {
        return Traits::read(*static_cast<const storage_type *>(state_->data()));
    }

    template <typename Value>
        requires(!details::scalar_type_traits<T>::is_collection) &&
                requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const
        noexcept(noexcept(*static_cast<storage_type *>(state_->mutable_data()) = std::forward<Value>(value))) {
        *static_cast<storage_type *>(state_->mutable_data()) = std::forward<Value>(value);
        state_->publish();
    }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        value.input_view().assign(
            state_->mutable_data(),
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type());
        state_->publish();
    }

    void invalidate() const { state_->invalidate(); }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<T, Properties...>;

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

static_assert(sizeof(Out<TS<int>>) == sizeof(void *));

} // namespace bg

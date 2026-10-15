#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/details/value_storage.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <concepts>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg {

template <details::scalar_variable Variable, auto... Properties>
class Out<TS<Variable, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        const auto &source = details::scalar_data(value);
        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
        if (state_->suppresses_equal_updates()) {
            if (!runtime_type.accepts_input(source.runtime_type())) {
                throw std::invalid_argument{"incompatible scalar runtime representations"};
            }
            if (valid() && runtime_type.equals(state_->data(), source)) {
                return;
            }
        }

        runtime_type.assign_from_input(state_->mutable_data(), source);
        state_->publish();
    }

    template <typename Value> void set_value(ScalarValue<Value> value) const {
        const auto &source = details::scalar_data(value);
        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
        if (state_->suppresses_equal_updates()) {
            if (!runtime_type.accepts_input(source.runtime_type())) {
                throw std::invalid_argument{"incompatible scalar runtime representations"};
            }
            if (valid() && runtime_type.equals(state_->data(), source)) {
                return;
            }
        }

        std::move(value).move_to(runtime_type, state_->mutable_data());
        state_->publish();
    }

    void invalidate() const { state_->invalidate(); }

    void apply(const literal::ScalarLiteral &literal) const {
        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
        set_value(ScalarValue<>::from_literal(runtime_type, literal));
    }

    template <typename Input, auto... TimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TS<Input, TimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() ||
            (!input.modified() && !input.became_invalid())) {
            return;
        }
        copy(input);
    }

    template <typename Input, auto... TimeSeriesProperties, auto... InputProperties>
    void copy(const In<TS<Input, TimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            invalidate();
            return;
        }
        set_value(input.value_view());
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<Variable, Properties...>;

  private:
    TimeSeriesOutputBinding state_;
};

template <typename T, auto... Properties>
class Out<TS<T, Properties...>> {
    using Traits = scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}

    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(!scalar_declaration_traits<T>::generic)
        : state_{last_updated,
                 &value,
                 cpp_time_series_runtime_type<TS<T, Properties...>, TimeSeriesTypeRole::output>(),
                 context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        requires(!scalar_declaration_traits<T>::generic)
        : state_{last_updated, &value, runtime_type, context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, void *output_path)
        requires(!scalar_declaration_traits<T>::generic)
        : state_{last_updated,
                 &value,
                 cpp_time_series_runtime_type<TS<T, Properties...>, TimeSeriesTypeRole::output>(),
                 output_path} {}

    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    value_type value() const
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T>)
    {
        if constexpr (!details::scalar_storage_supports_alternative_layouts<T>) {
            return Traits::read(*static_cast<const storage_type *>(state_->data()));
        } else {
            const auto &scalar_runtime_type =
                static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
            return Scalar<T>::from(scalar_runtime_type, state_->data()).value();
        }
    }

    Scalar<T> value() const
        requires details::hierarchical_scalar_declaration<T>
    {
        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
        return Scalar<T>::from(runtime_type, state_->data());
    }

    template <typename Value>
        requires(!scalar_declaration_traits<T>::is_collection && !details::hierarchical_scalar_declaration<T>) &&
                std::constructible_from<std::remove_cvref_t<value_type>, Value &&> &&
                (details::scalar_storage_has_borrow_value<T> ||
                 details::scalar_storage_supports_alternative_layouts<T> ||
                 requires(storage_type &target, const std::remove_cvref_t<value_type> &source, allocator &memory) {
                     Traits::assign(target, source, memory);
                 })
    void set(Value &&value) const {
        auto requested = std::remove_cvref_t<value_type>(std::forward<Value>(value));
        if constexpr (details::scalar_storage_has_borrow_value<T> ||
                      details::scalar_storage_supports_alternative_layouts<T>) {
            const auto source = details::make_scalar_source<T>(requested);
            set_scalar_data(source);
        } else {
            if constexpr (Traits::supports_equality) {
                if (state_->suppresses_equal_updates() && valid() && Traits::equals(this->value(), requested)) {
                    return;
                }
            }

            auto allocator = bg::allocator{};
            if constexpr (Traits::assign_uses_memory_resource) {
                allocator = state_->allocator();
            }
            Traits::assign(*static_cast<storage_type *>(state_->mutable_data()), requested, allocator);
            state_->publish();
        }
    }

    template <details::hierarchical_scalar_handle Value>
        requires details::hierarchical_scalar_declaration<T>
    void set(const Value &value) const {
        set_value(Scalar<typename Value::scalar_type>::from(value));
    }

    template <typename Value> void set_value(const Scalar<Value> &value) const {
        set_scalar_data(details::scalar_data(value));
    }

    template <typename Value> void set_value(ScalarValue<Value> value) const {
        auto output = details::output_factory::make<Out<TS<SCALAR>>>(state_.get());
        output.set_value(std::move(value));
    }

    void apply(const literal::ScalarLiteral &literal) const {
        auto output = details::output_factory::make<Out<TS<SCALAR>>>(state_.get());
        output.apply(literal);
    }

    template <auto... TimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TS<T, TimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() ||
            (!input.modified() && !input.became_invalid())) {
            return;
        }
        copy(input);
    }

    template <auto... TimeSeriesProperties, auto... InputProperties>
    void copy(const In<TS<T, TimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            invalidate();
            return;
        }

        if constexpr (scalar_declaration_traits<T>::is_collection || details::hierarchical_scalar_declaration<T> ||
                      details::scalar_uses_shared_handle<T>) {
            set_value(input.value_view());
        } else {
            set(input.value());
        }
    }

    void invalidate() const { state_->invalidate(); }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TS<T, Properties...>;

  private:
    TimeSeriesOutputBinding state_;

    void set_scalar_data(const details::ScalarData &source) const {
        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(state_->runtime_type()).scalar_runtime_type();
        if (state_->suppresses_equal_updates()) {
            if (!runtime_type.accepts_input(source.runtime_type())) {
                throw std::invalid_argument{"incompatible scalar runtime representations"};
            }
            if (valid() && runtime_type.equals(state_->data(), source)) {
                return;
            }
        }

        runtime_type.assign_from_input(state_->mutable_data(), source);
        state_->publish();
    }
};

static_assert(sizeof(Out<TS<i64>>) == sizeof(void *));

} // namespace bg

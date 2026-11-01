#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/schema_variable.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <stdexcept>

namespace bg {
namespace details {

struct bundle_output_fields {
    template <typename Declaration, auto...>
    using type = Out<typename bundle_field_projection<Declaration>::time_series_type>;
};

class bundle_output_factory {
  public:
    explicit bundle_output_factory(const OutputState &state) noexcept : state_{&state} {}

    template <typename SchemaField> [[nodiscard]] auto make(std::size_t index) const {
        using Item = typename SchemaField::time_series_type;
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return output_factory::make<Out<Item>>(item.last_updated, item.value, item.runtime_type, path);
    }

  private:
    const OutputState *state_;
};

template <template <template <typename, auto...> typename> typename Schema>
[[nodiscard]] auto make_bundle_outputs(const OutputState &state) {
    auto factory = bundle_output_factory{state};
    return make_bundle_fields<Schema, bundle_output_fields::template type>(factory);
}

} // namespace details

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
    requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
class Out<TSB<Schema, Properties...>> {
  public:
    using time_series_type = TSB<Schema, Properties...>;
    using fields_type = Schema<details::bundle_output_fields::template type>;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept
        : state_(std::move(state)), fields_{details::make_bundle_outputs<Schema>(state_.get())} {}
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    const fields_type *operator->() const noexcept { return &fields_; }
    const fields_type &fields() const noexcept { return fields_; }
    void invalidate() const { state_->invalidate(); }

    void apply(const literal::TimeSeriesLiteral &literal) const {
        details::apply_time_series_literal(details::output_factory::make<Out<TS_T>>(state_.get()), literal);
    }

    template <template <template <typename, auto...> typename> typename InputSchema,
              auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TSB<InputSchema, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        state_->runtime_type().copy_tick(input.input_data(), state_.get());
    }

    template <template <template <typename, auto...> typename> typename InputSchema,
              auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy(const In<TSB<InputSchema, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        state_->runtime_type().copy(input.input_data(), state_.get());
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

  private:
    TimeSeriesOutputBinding state_;
    fields_type fields_;
};

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
    requires details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>
class Out<TSB<Schema, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    std::size_t size() const noexcept { return state_->runtime_type().item_count(state_->data()); }
    util::symbol item_name(std::size_t index) const {
        return static_cast<const TimeSeriesBundleType &>(state_->runtime_type().type()).item_name(index);
    }
    Out<TS_T> operator[](std::size_t index) const {
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return details::output_factory::make<Out<TS_T>>(item.last_updated, item.value, item.runtime_type, path);
    }

    void invalidate() const { state_->invalidate(); }

    void apply(const literal::TimeSeriesLiteral &literal) const {
        const auto sentinel = literal::Sentinel::from(literal);
        if (sentinel) {
            if (*sentinel == literal::SKIP) {
                return;
            }
            if (*sentinel == literal::INVALID) {
                invalidate();
                return;
            }
        }
        apply_values(details::scalar_literal::sequence(literal));
    }

  private:
    void apply_values(std::span<const literal::ScalarLiteral> values) const {
        const auto field_index = [&](std::string_view expected) {
            for (auto index = std::size_t{}; index < size(); ++index) {
                if (item_name(index).text() == expected) {
                    return index;
                }
            }
            throw std::invalid_argument{"a TSB update contains an unknown field"};
        };

        for (auto position = std::size_t{}; position < values.size(); ++position) {
            const auto current = details::scalar_literal::pair(
                values[position], "a TSB update entry requires a field name and a value");
            const auto current_name = details::scalar_literal::field_name(*current.first);
            (void)field_index(current_name);

            for (auto previous = std::size_t{}; previous < position; ++previous) {
                const auto prior = details::scalar_literal::pair(
                    values[previous], "a TSB update entry requires a field name and a value");
                if (details::scalar_literal::field_name(*prior.first) == current_name) {
                    throw std::invalid_argument{"a TSB update contains a duplicate field"};
                }
            }
        }

        for (const auto &value : values) {
            const auto current = details::scalar_literal::pair(
                value, "a TSB update entry requires a field name and a value");
            const auto index = field_index(details::scalar_literal::field_name(*current.first));
            details::apply_time_series_literal((*this)[index], *current.second);
        }
    }

  public:
    template <template <template <typename, auto...> typename> typename InputSchema,
              auto... InputTimeSeriesProperties, auto... InputProperties>
        requires details::bundle_schema_variable_declaration<InputSchema<details::bundle_schema_field>>
    void copy_tick(const In<TSB<InputSchema, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() || !input.modified()) {
            return;
        }
        if (!input.valid()) {
            invalidate();
            return;
        }

        input.for_each_updated([&](item_id_t id) {
            if (id < size()) {
                (*this)[id].copy_tick(input[id]);
            }
        });
    }

    template <template <template <typename, auto...> typename> typename InputSchema,
              auto... InputTimeSeriesProperties, auto... InputProperties>
        requires details::bundle_schema_variable_declaration<InputSchema<details::bundle_schema_field>>
    void copy(const In<TSB<InputSchema, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            invalidate();
            return;
        }

        for (auto index = std::size_t{}; index < size(); ++index) {
            (*this)[index].copy(input[index]);
        }
    }
    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSB<Schema, Properties...>;

  private:
    TimeSeriesOutputBinding state_;
};

} // namespace bg

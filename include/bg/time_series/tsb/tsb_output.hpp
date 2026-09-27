#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>

namespace bg {
namespace details {

template <typename TimeSeries> using bundle_output_field = Out<TimeSeries>;

class bundle_output_factory {
  public:
    explicit bundle_output_factory(const OutputState &state) noexcept : state_{&state} {}

    template <typename SchemaField> [[nodiscard]] auto make(std::size_t index) const {
        using Item = typename SchemaField::time_series_type;
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return output_factory::make<Item>(item.last_updated, item.value, item.runtime_type, path);
    }

  private:
    const OutputState *state_;
};

template <template <template <typename> typename> typename Schema>
[[nodiscard]] auto make_bundle_outputs(const OutputState &state) {
    auto factory = bundle_output_factory{state};
    return make_bundle_fields<Schema, bundle_output_field>(factory);
}

} // namespace details

template <template <template <typename> typename> typename Schema, auto... Properties>
    requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
class Out<TSB<Schema, Properties...>> {
  public:
    using time_series_type = TSB<Schema, Properties...>;
    using fields_type = Schema<details::bundle_output_field>;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept
        : state_(Proxy::borrow(state)), fields_{details::make_bundle_outputs<Schema>(state_.get())} {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{runtime_type}, context)),
          fields_{bind(last_updated, value, context)} {}

    Out(EvaluationTime &last_updated, void *value, const TimeSeriesRuntimeType &runtime_type, void *output_path)
        : state_(Proxy::copy(details::OutputState{runtime_type}, output_path)),
          fields_{bind(last_updated, value, output_path)} {}

  public:
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const fields_type *operator->() const noexcept { return &fields_; }
    const fields_type &fields() const noexcept { return fields_; }
    void invalidate() const { state_->invalidate(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    template <typename Owner> [[nodiscard]] fields_type bind(EvaluationTime &last_updated, void *value, Owner &owner) {
        state_->bind(last_updated, value, owner);
        return details::make_bundle_outputs<Schema>(state_.get());
    }

    Proxy state_;
    fields_type fields_;
};

template <template <template <typename> typename> typename Schema, auto... Properties>
    requires details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>
class Out<TSB<Schema, Properties...>> {
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
    std::size_t size() const noexcept { return state_->runtime_type().item_count(state_->data()); }
    util::symbol item_name(std::size_t index) const {
        return static_cast<const TimeSeriesBundleType &>(state_->runtime_type().type()).item_name(index);
    }
    Out<TS_T> operator[](std::size_t index) const {
        const auto id = static_cast<item_id_t>(index);
        const auto item = state_->runtime_type().item(state_->mutable_data(), id);
        auto *path = state_->add_path(state_->runtime_type(), id);
        return details::output_factory::make<TS_T>(item.last_updated, item.value, item.runtime_type, path);
    }

    void invalidate() const { state_->invalidate(); }
    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSB<Schema, Properties...>;

  private:
    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

} // namespace bg

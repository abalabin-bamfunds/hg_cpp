#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/details/output_proxy.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>

namespace bg {

template <typename K, auto... Properties> class Out<TSS<K, Properties...>> {
  public:
    using storage_type = details::time_series_set_storage;

  private:
    friend details::output_factory;

    explicit Out(details::OutputState &state) noexcept : state_(Proxy::borrow(state)) {}

    Out(EvaluationTime &last_updated, storage_type &value, EvaluationContext &context)
        : state_(Proxy::copy(details::OutputState{cpp_time_series_runtime_type<TSS<K, Properties...>>()}, context)) {
        state_->bind(last_updated, &value, context);
    }

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
    bool insert(const Scalar<K> &item) const { return insert(details::scalar_data(item)); }
    bool erase(const Scalar<K> &item) const { return erase(details::scalar_data(item)); }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool insert(Source &&item) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(item));
        return insert(details::make_scalar_source<K>(value));
    }

    template <typename Source>
        requires(!details::scalar_variable<K> && scalar_source_for<K, Source>)
    bool erase(Source &&item) const {
        const auto value = scalar_source_value<K>(std::forward<Source>(item));
        return erase(details::make_scalar_source<K>(value));
    }

    void clear() const {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        const auto should_publish = !valid() || value.size() != 0;
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        value.clear(key_runtime_type(), key_layout(), memory, [](std::uint32_t) noexcept {});

        if (should_publish) {
            state_->publish();
        }
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<TS_T>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSS<K, Properties...>;
    using item_type = K;

  private:
    details::time_series_set_storage &storage() const noexcept {
        return *static_cast<details::time_series_set_storage *>(state_->mutable_data());
    }

    const ScalarRuntimeType &key_runtime_type() const noexcept {
        return static_cast<const details::TimeSeriesSetRuntimeType &>(state_->runtime_type()).key_runtime_type();
    }

    util::runtime_item_layout key_layout() const noexcept {
        return {key_runtime_type().size(), key_runtime_type().alignment()};
    }

    bool insert(const details::ScalarData &item) const {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result =
            value.insert(item, key_runtime_type(), key_layout(), memory, [](std::uint32_t) {}, [](std::uint32_t) {});
        if (result.change != details::time_series_set_change::none) {
            state_->publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    bool erase(const details::ScalarData &item) const {
        auto &memory = details::current_scalar_memory_resource();
        auto &value = storage();
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.erase(item, key_runtime_type(), key_layout(), memory, [](std::uint32_t) noexcept {});
        if (result.change != details::time_series_set_change::none) {
            state_->publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    using Proxy = details::output_proxy<details::OutputState>;

    Proxy state_;
};

} // namespace bg

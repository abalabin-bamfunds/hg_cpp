#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/details/scalar_literal.hpp>
#include <bg/scalar/canonical.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/time_series/tss/tss_type.hpp>

namespace bg {

template <typename K, auto... Properties> class Out<TSS<K, Properties...>> {
  public:
    using storage_type = details::time_series_set_storage;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(TimeSeriesState &last_updated, storage_type &value, EvaluationContext &context)
        requires(!details::generic_time_series<TSS<K, Properties...>>)
        : state_{last_updated,
                 &value,
                 cpp_time_series_runtime_type<TSS<K, Properties...>, TimeSeriesTypeRole::output>(),
                 context} {}

    Out(TimeSeriesState &last_updated, storage_type &value, const TimeSeriesRuntimeType &runtime_type,
        EvaluationContext &context)
        requires(!details::generic_time_series<TSS<K, Properties...>>)
        : state_{last_updated, &value, runtime_type, context} {}

    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    void invalidate() const { state_->invalidate(); }

    void apply(const literal::TimeSeriesLiteral &literal) const {
        const auto sentinel = literal::Sentinel::from(literal);
        if (sentinel) {
            if (*sentinel == literal::SKIP) {
                return;
            }
            if (*sentinel == literal::INVALID) {
                throw std::invalid_argument{"INVALID is not valid as a TSS update"};
            }
            if (*sentinel == literal::REMOVE) {
                throw std::invalid_argument{"REMOVE is only valid as a TSD value"};
            }
            if (*sentinel == literal::REMOVED) {
                throw std::invalid_argument{"a time-series collection update requires a sequence"};
            }
            throw std::invalid_argument{"a time-series collection update requires a sequence"};
        }
        apply_values(details::scalar_literal::sequence(literal));
    }

    bool insert(const Scalar<K> &item) const { return insert(details::scalar_data(item)); }
    bool erase(const Scalar<K> &item) const { return erase(details::scalar_data(item)); }

    bool insert(const Scalar<> &item) const
        requires details::scalar_variable<K>
    {
        return insert(details::scalar_data(item));
    }

    bool erase(const Scalar<> &item) const
        requires details::scalar_variable<K>
    {
        return erase(details::scalar_data(item));
    }

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
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        const auto should_publish = !valid() || value.size() != 0;
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        value.clear(key_runtime_type(), key_layout(), memory, [](std::uint32_t) noexcept {});

        if (should_publish) {
            state_->publish();
        }
    }

    template <auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy_tick(const In<TSS<K, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() ||
            (!input.modified() && !input.became_invalid())) {
            return;
        }
        if (!input.valid() && !valid()) {
            return;
        }
        if (input.valid() && !valid()) {
            clear();
        }

        input.for_each_added([&](const auto &item) { insert(item); });
        input.for_each_removed([&](const auto &item) { erase(item); });
    }

    template <auto... InputTimeSeriesProperties, auto... InputProperties>
    void copy(const In<TSS<K, InputTimeSeriesProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            if (valid()) {
                clear();
            }
            return;
        }

        const auto &runtime = static_cast<const details::TimeSeriesSetRuntimeType &>(state_->runtime_type());
        if (!valid()) {
            clear();
        }

        auto id = runtime.set_next_item_id(state_->mutable_last_updated(), state_->data(),
                                           details::TimeSeriesSetRuntimeType::Range::current, 0);
        while (id != util::occupancy_index::npos) {
            const auto next_id = runtime.set_next_item_id(state_->mutable_last_updated(), state_->data(),
                                                         details::TimeSeriesSetRuntimeType::Range::current, id + 1);
            const auto item = Scalar<K>::from(key_runtime_type(), runtime.set_key(state_->data(), id));
            if (!input.contains(item)) {
                erase(item);
            }
            id = next_id;
        }

        input.for_each([&](const auto &item) { insert(item); });
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = TSS<K, Properties...>;
    using item_type = K;

  private:
    void apply_values(std::span<const literal::ScalarLiteral> values) const {
        if (values.empty()) {
            if (!valid()) {
                clear();
            }
            return;
        }

        for (const auto &value : values) {
            if (const auto sentinel = literal::Sentinel::from(value)) {
                if (*sentinel == literal::SKIP) {
                    continue;
                }
                if (*sentinel == literal::INVALID) {
                    throw std::invalid_argument{"INVALID is not a TSS item"};
                }
                if (*sentinel == literal::REMOVE) {
                    throw std::invalid_argument{"REMOVE is only valid as a TSD value"};
                }
                if (*sentinel == literal::REMOVED) {
                    throw std::invalid_argument{"REMOVED requires an argument"};
                }
            }
            if (const auto tagged = literal::TaggedLiteral::from(value)) {
                if (tagged->sentinel == literal::REMOVED) {
                    details::scalar_literal::with_scalar_data(
                        key_runtime_type(), tagged->argument,
                        [&](const details::ScalarData &item) { (void)erase(item); });
                    continue;
                }
            }
            details::scalar_literal::with_scalar_data(
                key_runtime_type(), value, [&](const details::ScalarData &item) { (void)insert(item); });
        }
    }

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
        auto memory = details::current_scalar_allocator();
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
        auto memory = details::current_scalar_allocator();
        auto &value = storage();
        value.reset_change_tracking(state_->context().evaluation_time(), key_runtime_type(), key_layout(), memory,
                                    [](std::uint32_t) noexcept {});
        const auto result = value.erase(item, key_runtime_type(), key_layout(), memory, [](std::uint32_t) noexcept {});
        if (result.change != details::time_series_set_change::none) {
            state_->publish();
        }
        return result.change != details::time_series_set_change::none;
    }

    TimeSeriesOutputBinding state_;
};

} // namespace bg

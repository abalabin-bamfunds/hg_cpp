#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/time_series/time_series_type.hpp>

namespace bg {

template <typename Target, auto... Properties> class Out<REF<Target, Properties...>> {
  public:
    using storage_type = Reference;

  public:
    explicit Out(TimeSeriesOutputBinding state) noexcept : state_(std::move(state)) {}
    Out(Out &&) noexcept = default;
    Out(const Out &) = delete;

    bool valid() const noexcept { return state_->valid(); }
    EvaluationTime last_updated() const noexcept { return state_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return state_->runtime_type(); }
    Reference value() const noexcept { return stored_reference(); }

    template <typename InputTarget, auto... ReferenceProperties, auto... InputProperties>
    void copy_tick(const In<REF<InputTarget, ReferenceProperties...>, InputProperties...> &input) const {
        if (input.last_updated() != state_->evaluation_time() || !input.modified()) {
            return;
        }
        copy(input);
    }

    template <typename InputTarget, auto... ReferenceProperties, auto... InputProperties>
    void copy(const In<REF<InputTarget, ReferenceProperties...>, InputProperties...> &input) const {
        if (!input.valid()) {
            invalidate();
            return;
        }
        set_value(input.value());
    }

    void set_value(Reference value) const {
        if (!value) {
            invalidate();
            return;
        }
        if (state_->suppresses_equal_updates() && valid() && stored_reference() == value) {
            return;
        }

        stored_reference() = value;
        state_->publish();
    }

    void invalidate() const {
        stored_reference() = nullptr;
        state_->invalidate_value();
    }

    const Out *operator->() const noexcept { return this; }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(state_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = REF<Target, Properties...>;
    using target_type = Target;

  private:
    Reference &stored_reference() const noexcept { return *static_cast<Reference *>(state_->mutable_data()); }

    TimeSeriesOutputBinding state_;
};

} // namespace bg

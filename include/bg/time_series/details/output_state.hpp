#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/details/output_path.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>

namespace bg {

// Connects an output view to its value storage and the mutation path of any containing collections.
class OutputState {
  public:
    OutputState() = default;
    explicit OutputState(const TimeSeriesRuntimeType &runtime_type) noexcept
        : runtime_type_{&runtime_type, *runtime_type.type().guarantee().value()} {}

    void bind(TimeSeriesState &last_updated, void *data, const EvaluationTime &evaluation_time) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(&evaluation_time);
    }

    void bind(TimeSeriesState &last_updated, void *data, EvaluationContext &context) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(&context);
    }

    void bind(TimeSeriesState &last_updated, void *data, void *output_path) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(static_cast<details::OutputPath *>(output_path));
    }

    void release() noexcept {
        if (owner_.holds<const EvaluationTime>()) {
            return;
        }

        auto &evaluation_context = context();
        auto *path = owner_.get<details::OutputPath>();
        evaluation_context.scratch().release_last(this, sizeof(OutputState));
        if (path != nullptr) {
            details::release_output_path(path);
        }
    }

    void release_path() noexcept {
        if (auto *path = owner_.get<details::OutputPath>()) {
            details::release_output_path(path);
        }
    }

    bool valid() const noexcept { return last_updated_->valid(); }
    EvaluationTime last_updated() const noexcept { return last_updated_->time(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    bool suppresses_equal_updates() const noexcept { return runtime_type_.tag() == TimeSeriesGuarantee::deduplicated; }
    const void *data() const noexcept { return data_; }
    void *mutable_data() const noexcept { return const_cast<void *>(data_); }
    TimeSeriesState &mutable_last_updated() const noexcept { return *const_cast<TimeSeriesState *>(last_updated_); }

    void invalidate() const { runtime_type().invalidate_output(*this); }

    void invalidate_value() const noexcept {
        if (!valid()) {
            return;
        }
        mutable_last_updated().invalidate(evaluation_time());
        publish_containing_collections();
    }

    void publish() const noexcept {
        mutable_last_updated().publish(evaluation_time());
        publish_containing_collections();
    }

    [[nodiscard]] void *add_path(const TimeSeriesRuntimeType &runtime_type, item_id_t id) const {
        auto *parent = owner_.get<details::OutputPath>();
        return details::add_output_path(context(), runtime_type, mutable_last_updated(), mutable_data(), id, parent);
    }

    void publish_item(const TimeSeriesRuntimeType &runtime_type, item_id_t id) const {
        auto *path = add_path(runtime_type, id);
        details::publish_output_path(path);
        details::release_output_path(path);
    }

    EvaluationContext &context() const noexcept {
        if (auto *evaluation_context = owner_.get<EvaluationContext>()) {
            return *evaluation_context;
        }
        return details::output_path_context(owner_.get<details::OutputPath>());
    }

    bg::allocator &allocator() const noexcept { return details::current_scalar_allocator(); }

  private:
    void publish_containing_collections() const noexcept {
        if (auto *path = owner_.get<details::OutputPath>()) {
            details::publish_output_path(path);
        }
    }

    const EvaluationTime &evaluation_time() const noexcept {
        if (auto *time = owner_.get<const EvaluationTime>()) {
            return *time;
        }
        return context().evaluation_time();
    }

    util::tagged_ptr<const TimeSeriesRuntimeType, 2, TimeSeriesGuarantee> runtime_type_{};
    const TimeSeriesState *last_updated_{&initial_time_series_state};
    const void *data_{};
    util::variant_ptr_with_alignment<4, details::OutputPath, EvaluationContext, const EvaluationTime> owner_{&MIN_DT};
};

static_assert(sizeof(OutputState) == 4 * sizeof(void *));
static_assert(alignof(TimeSeriesRuntimeType) >= 4);
static_assert(alignof(EvaluationContext) >= 4);
static_assert(alignof(EvaluationTime) >= 4);

} // namespace bg

namespace bg::details {

using ::bg::OutputState;

} // namespace bg::details

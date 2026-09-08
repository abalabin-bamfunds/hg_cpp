#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/time_series/details/output_path.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>

namespace bg::details {

// Connects an output view to its value storage and the mutation path of any containing collections.
class OutputState {
  public:
    OutputState() = default;
    explicit OutputState(const TimeSeriesRuntimeType &runtime_type) noexcept
        : runtime_type_{&runtime_type, *runtime_type.type().guarantee().value()} {}

    void bind(EvaluationTime &last_updated, void *data, const EvaluationTime &evaluation_time) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(&evaluation_time, Owner::evaluation_time);
    }

    void bind(EvaluationTime &last_updated, void *data, EvaluationContext &context) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(&context, Owner::context);
    }

    void bind(EvaluationTime &last_updated, void *data, void *output_path) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_.reset(output_path, Owner::path);
    }

    void release() noexcept {
        if (owner_.tag() == Owner::evaluation_time) {
            return;
        }

        auto &evaluation_context = context();
        auto *path = owner_.tag() == Owner::path ? owner_.get<void>() : nullptr;
        evaluation_context.scratch().release_last(this, sizeof(OutputState));
        if (path != nullptr) {
            release_output_path(path);
        }
    }

    void release_path() noexcept {
        if (owner_.tag() == Owner::path && owner_) {
            release_output_path(owner_.get<void>());
        }
    }

    bool valid() const noexcept { return *last_updated_ > MIN_DT; }
    EvaluationTime last_updated() const noexcept { return *last_updated_; }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    bool suppresses_equal_updates() const noexcept { return runtime_type_.tag() == TimeSeriesGuarantee::deduplicated; }
    const void *data() const noexcept { return data_; }
    void *mutable_data() const noexcept { return const_cast<void *>(data_); }
    EvaluationTime &mutable_last_updated() const noexcept { return *const_cast<EvaluationTime *>(last_updated_); }

    void invalidate() const { runtime_type().invalidate_output(*this); }

    void invalidate_value() const noexcept {
        if (!valid()) {
            return;
        }
        mutable_last_updated() = MIN_DT;
        publish_containing_collections();
    }

    void publish() const noexcept {
        mutable_last_updated() = evaluation_time();
        publish_containing_collections();
    }

    [[nodiscard]] void *add_path(const TimeSeriesRuntimeType &runtime_type, TimeSeriesItemId id) const {
        auto *parent = owner_.tag() == Owner::path ? owner_.get<void>() : nullptr;
        return add_output_path(context(), runtime_type, mutable_last_updated(), mutable_data(), id, parent);
    }

    void publish_item(const TimeSeriesRuntimeType &runtime_type, TimeSeriesItemId id) const {
        auto *path = add_path(runtime_type, id);
        publish_output_path(path);
        release_output_path(path);
    }

    EvaluationContext &context() const noexcept {
        if (owner_.tag() == Owner::context) {
            return *owner_.get<EvaluationContext>();
        }
        return output_path_context(owner_.get<void>());
    }

  private:
    void publish_containing_collections() const noexcept {
        if (owner_.tag() == Owner::path && owner_) {
            publish_output_path(owner_.get<void>());
        }
    }

    const EvaluationTime &evaluation_time() const noexcept {
        if (owner_.tag() == Owner::evaluation_time) {
            return *owner_.get<const EvaluationTime>();
        }
        return context().evaluation_time();
    }

    enum class Owner : std::uintptr_t {
        path,
        context,
        evaluation_time,
    };

    util::tagged_ptr<const TimeSeriesRuntimeType, 2, TimeSeriesGuarantee> runtime_type_{};
    const EvaluationTime *last_updated_{&MIN_DT};
    const void *data_{};
    util::erased_tagged_ptr<4, 2, Owner> owner_{&MIN_DT, Owner::evaluation_time};
};

static_assert(sizeof(OutputState) == 4 * sizeof(void *));
static_assert(alignof(TimeSeriesRuntimeType) >= 4);
static_assert(alignof(EvaluationContext) >= 4);
static_assert(alignof(EvaluationTime) >= 4);

} // namespace bg::details

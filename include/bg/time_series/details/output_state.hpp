#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/time_series/details/output_path.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <cstdint>

namespace bg::details {

// Connects an output view to its value storage and the mutation path of any containing collections.
class OutputState {
  public:
    OutputState() = default;
    explicit OutputState(const TimeSeriesRuntimeType &runtime_type) noexcept : runtime_type_{&runtime_type} {}

    void bind(EvaluationTime &last_updated, void *data, const EvaluationTime &evaluation_time) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_ = tagged(evaluation_time, evaluation_time_tag);
    }

    void bind(EvaluationTime &last_updated, void *data, EvaluationContext &context) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_ = tagged(context, context_tag);
    }

    void bind(EvaluationTime &last_updated, void *data, void *output_path) noexcept {
        last_updated_ = &last_updated;
        data_ = data;
        owner_ = reinterpret_cast<std::uintptr_t>(output_path);
    }

    void release() noexcept {
        if (tag() == evaluation_time_tag) {
            return;
        }

        auto &evaluation_context = context();
        auto *path = tag() == path_tag ? reinterpret_cast<void *>(owner_) : nullptr;
        evaluation_context.scratch().release_last(this, sizeof(OutputState));
        if (path != nullptr) {
            release_output_path(path);
        }
    }

    void release_path() noexcept {
        if (tag() == path_tag && owner_ != 0) {
            release_output_path(reinterpret_cast<void *>(owner_));
        }
    }

    [[nodiscard]] bool valid() const noexcept { return *last_updated_ > MIN_DT; }
    [[nodiscard]] EvaluationTime last_updated() const noexcept { return *last_updated_; }
    [[nodiscard]] const TimeSeriesRuntimeType &runtime_type() const noexcept { return *runtime_type_; }
    [[nodiscard]] const void *data() const noexcept { return data_; }
    [[nodiscard]] void *mutable_data() const noexcept { return const_cast<void *>(data_); }
    [[nodiscard]] EvaluationTime &mutable_last_updated() const noexcept {
        return *const_cast<EvaluationTime *>(last_updated_);
    }

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
        auto *parent = tag() == path_tag ? reinterpret_cast<void *>(owner_) : nullptr;
        return add_output_path(context(), runtime_type, mutable_last_updated(), mutable_data(), id, parent);
    }

    void publish_item(const TimeSeriesRuntimeType &runtime_type, TimeSeriesItemId id) const {
        auto *path = add_path(runtime_type, id);
        publish_output_path(path);
        release_output_path(path);
    }

    [[nodiscard]] EvaluationContext &context() const noexcept {
        if (tag() == context_tag) {
            return *untagged<EvaluationContext>();
        }
        return output_path_context(reinterpret_cast<void *>(owner_));
    }

  private:
    void publish_containing_collections() const noexcept {
        if (tag() == path_tag && owner_ != 0) {
            publish_output_path(reinterpret_cast<void *>(owner_));
        }
    }

    [[nodiscard]] const EvaluationTime &evaluation_time() const noexcept {
        if (tag() == evaluation_time_tag) {
            return *untagged<EvaluationTime>();
        }
        return context().evaluation_time();
    }

    template <typename T> [[nodiscard]] static std::uintptr_t tagged(const T &value, std::uintptr_t tag) noexcept {
        return reinterpret_cast<std::uintptr_t>(&value) | tag;
    }

    template <typename T> [[nodiscard]] T *untagged() const noexcept {
        return reinterpret_cast<T *>(owner_ & ~tag_mask);
    }

    [[nodiscard]] std::uintptr_t tag() const noexcept { return owner_ & tag_mask; }

    static constexpr auto path_tag = std::uintptr_t{};
    static constexpr auto context_tag = std::uintptr_t{1};
    static constexpr auto evaluation_time_tag = std::uintptr_t{2};
    static constexpr auto tag_mask = std::uintptr_t{3};

    const TimeSeriesRuntimeType *runtime_type_{};
    const EvaluationTime *last_updated_{&MIN_DT};
    const void *data_{};
    std::uintptr_t owner_{tagged(MIN_DT, evaluation_time_tag)};
};

static_assert(sizeof(OutputState) == 4 * sizeof(void *));
static_assert(alignof(EvaluationContext) >= 4);
static_assert(alignof(EvaluationTime) >= 4);

} // namespace bg::details

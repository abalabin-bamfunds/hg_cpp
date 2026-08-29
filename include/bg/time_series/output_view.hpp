#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/view.hpp>

#include <cstdint>

namespace bg {

class TimeSeriesOutputView : public TimeSeriesView {
  public:
    void bind(EvaluationTime &last_updated, void *value, const EvaluationTime &evaluation_time) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
        owner_ = tagged(evaluation_time, evaluation_time_tag);
    }

    void bind(EvaluationTime &last_updated, void *value, EvaluationContext &context) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
        owner_ = tagged(context, context_tag);
    }

    void bind(EvaluationTime &last_updated, void *value, void *output_mutation) noexcept {
        last_updated_ = &last_updated;
        value_ = value;
        owner_ = reinterpret_cast<std::uintptr_t>(output_mutation);
    }

    virtual void invalidate() const {
        if (!valid()) {
            return;
        }

        *mutable_last_updated() = MIN_DT;
        publish_containing_collections();
    }

  protected:
    [[nodiscard]] EvaluationTime *mutable_last_updated() const noexcept {
        return const_cast<EvaluationTime *>(last_updated_);
    }

    [[nodiscard]] void *mutable_value() const noexcept { return const_cast<void *>(value_); }

    void publish() const noexcept {
        *mutable_last_updated() = evaluation_time();
        publish_containing_collections();
    }

    [[nodiscard]] void *add_output_mutation(const TimeSeriesStorageRepresentation &representation,
                                            std::size_t item) const {
        auto *parent = tag() == mutation_tag ? reinterpret_cast<void *>(owner_) : nullptr;
        return context().add_output_mutation(representation, *mutable_last_updated(), mutable_value(), item, parent);
    }

    void publish_item(const TimeSeriesStorageRepresentation &representation, std::size_t item) const {
        context().publish_output_mutation(add_output_mutation(representation, item));
    }

    [[nodiscard]] EvaluationContext &context() const noexcept {
        if (tag() == context_tag) {
            return *untagged<EvaluationContext>();
        }

        return EvaluationContext::output_mutation_context(reinterpret_cast<void *>(owner_));
    }

  private:
    void publish_containing_collections() const noexcept {
        if (tag() == mutation_tag && owner_ != 0) {
            context().publish_output_mutation(reinterpret_cast<void *>(owner_));
        }
    }

    [[nodiscard]] const EvaluationTime &evaluation_time() const noexcept {
        if (tag() == evaluation_time_tag) {
            return *untagged<EvaluationTime>();
        }

        return context().evaluation_time();
    }

    template <typename T> [[nodiscard]] static std::uintptr_t tagged(const T &value, std::uintptr_t tag) noexcept {
        const auto address = reinterpret_cast<std::uintptr_t>(&value);
        return address | tag;
    }

    template <typename T> [[nodiscard]] T *untagged() const noexcept {
        return reinterpret_cast<T *>(owner_ & ~tag_mask);
    }

    [[nodiscard]] std::uintptr_t tag() const noexcept { return owner_ & tag_mask; }

    static constexpr auto mutation_tag = std::uintptr_t{};
    static constexpr auto context_tag = std::uintptr_t{1};
    static constexpr auto evaluation_time_tag = std::uintptr_t{2};
    static constexpr auto tag_mask = std::uintptr_t{3};

    std::uintptr_t owner_{tagged(MIN_DT, evaluation_time_tag)};
};

class TimeSeriesValueOutputView : public TimeSeriesOutputView {
  public:
    virtual void set_value(const ScalarInputView &value) const = 0;
};

template <typename T> class TypedTimeSeriesValueOutputView final : public TimeSeriesValueOutputView {
    using Traits = details::scalar_storage_traits<T>;

  public:
    using storage_type = typename Traits::stored_type;
    using value_type = typename Traits::value_type;

    [[nodiscard]] value_type value() const noexcept { return Traits::read(*static_cast<const storage_type *>(value_)); }

    template <typename Value>
        requires requires(storage_type &target, Value &&source) { target = std::forward<Value>(source); }
    void set(Value &&value) const
        noexcept(noexcept(*static_cast<storage_type *>(mutable_value()) = std::forward<Value>(value))) {
        *static_cast<storage_type *>(mutable_value()) = std::forward<Value>(value);
        publish();
    }

    void set_value(const ScalarInputView &value) const override;
};

static_assert(sizeof(TimeSeriesOutputView) == 4 * sizeof(void *));
static_assert(sizeof(TimeSeriesValueOutputView) == sizeof(TimeSeriesOutputView));
static_assert(alignof(EvaluationContext) >= 4);
static_assert(alignof(EvaluationTime) >= 4);

} // namespace bg

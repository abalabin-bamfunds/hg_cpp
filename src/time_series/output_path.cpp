#include <bg/time_series/details/output_path.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

#include <cstdint>
#include <memory>

namespace bg::details {

namespace {

struct OutputPath {
    OutputPath(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type, EvaluationTime &last_updated,
               void *value, TimeSeriesItemId id, OutputPath *parent) noexcept
        : owner{parent == nullptr ? reinterpret_cast<std::uintptr_t>(&context) | context_tag
                                  : reinterpret_cast<std::uintptr_t>(parent)},
          runtime_type{&runtime_type}, last_updated{&last_updated}, value{value}, id{id} {}

    [[nodiscard]] EvaluationContext &context() const noexcept {
        if ((owner & context_tag) != 0) {
            return *reinterpret_cast<EvaluationContext *>(owner & ~context_tag);
        }
        return reinterpret_cast<OutputPath *>(owner)->context();
    }

    [[nodiscard]] OutputPath *parent() const noexcept {
        return (owner & context_tag) == 0 ? reinterpret_cast<OutputPath *>(owner) : nullptr;
    }

    static constexpr auto context_tag = std::uintptr_t{1};

    std::uintptr_t owner;
    const TimeSeriesRuntimeType *runtime_type;
    EvaluationTime *last_updated;
    void *value;
    TimeSeriesItemId id;
};

static_assert(sizeof(OutputPath) == 40);
static_assert(alignof(EvaluationContext) >= 2);

} // namespace

void *add_output_path(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type,
                      EvaluationTime &last_updated, void *value, TimeSeriesItemId id, void *parent) {
    auto *storage = context.scratch().allocate(sizeof(OutputPath), alignof(OutputPath));
    return std::construct_at(static_cast<OutputPath *>(storage), context, runtime_type, last_updated, value, id,
                             static_cast<OutputPath *>(parent));
}

EvaluationContext &output_path_context(void *path) noexcept {
    return static_cast<OutputPath *>(path)->context();
}

void publish_output_path(void *output_path) noexcept {
    auto *path = static_cast<OutputPath *>(output_path);
    while (path != nullptr) {
        auto &entry = *path;
        auto &context = entry.context();
        const auto modified = entry.runtime_type->mark_item_modified(
            entry.value, entry.id, context.evaluation_time(), *entry.last_updated, current_scalar_memory_resource());
        *entry.last_updated = entry.runtime_type->collection_valid(entry.value) ? context.evaluation_time() : MIN_DT;
        if (!modified) {
            return;
        }
        path = entry.parent();
    }
}

void release_output_path(void *path) noexcept {
    auto &output_path = *static_cast<OutputPath *>(path);
    output_path.context().scratch().release_last(&output_path, sizeof(OutputPath));
}

} // namespace bg::details

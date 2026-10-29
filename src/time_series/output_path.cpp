#include <bg/time_series/details/output_path.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>
#include <memory>

namespace bg::details {

class OutputPath {
  public:
    OutputPath(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type, TimeSeriesState &last_updated,
               void *value, item_id_t id, OutputPath *parent) noexcept
        : runtime_type{&runtime_type}, last_updated{&last_updated}, value{value}, id{id} {
        if (parent == nullptr) {
            owner.reset(&context);
        } else {
            owner.reset(parent);
        }
    }

    EvaluationContext &context() const noexcept {
        if (auto *evaluation_context = owner.get<EvaluationContext>()) {
            return *evaluation_context;
        }
        return owner.get<OutputPath>()->context();
    }

    OutputPath *parent() const noexcept {
        return owner.get<OutputPath>();
    }

    util::variant_ptr_with_alignment<alignof(void *), OutputPath, EvaluationContext> owner;
    const TimeSeriesRuntimeType *runtime_type;
    TimeSeriesState *last_updated;
    void *value;
    item_id_t id;
};

static_assert(sizeof(OutputPath) == 40);
static_assert(alignof(EvaluationContext) >= 2);

void *add_output_path(EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type,
                      TimeSeriesState &last_updated, void *value, item_id_t id, void *parent) {
    auto *storage = context.scratch().allocate(sizeof(OutputPath), alignof(OutputPath));
    return std::construct_at(static_cast<OutputPath *>(storage), context, runtime_type, last_updated, value, id,
                             static_cast<OutputPath *>(parent));
}

EvaluationContext &output_path_context(void *path) noexcept {
    return static_cast<OutputPath *>(path)->context();
}

void publish_output_path(void *output_path, bool item_was_valid) noexcept {
    auto *path = static_cast<OutputPath *>(output_path);
    while (path != nullptr) {
        auto &entry = *path;
        auto &context = entry.context();
        const auto now = context.evaluation_time();
        const auto parent_item_was_valid = entry.last_updated->valid();
        const auto modified = entry.runtime_type->mark_item_modified(
            entry.value, entry.id, now, item_was_valid, current_scalar_allocator());
        const auto is_valid = entry.runtime_type->collection_valid(entry.value);
        if (!modified && parent_item_was_valid == is_valid) {
            return;
        }
        entry.last_updated->update_collection_validity(now, is_valid);
        item_was_valid = parent_item_was_valid;
        path = entry.parent();
    }
}

void release_output_path(void *path) noexcept {
    auto &output_path = *static_cast<OutputPath *>(path);
    output_path.context().scratch().release_last(&output_path, sizeof(OutputPath));
}

} // namespace bg::details

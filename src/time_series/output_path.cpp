#include <bg/time_series/details/output_path.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>

#include <cstdint>
#include <memory>

namespace bg::details {

namespace {

struct OutputPath {
    OutputPath(EvaluationContext &context, const TimeSeriesStorageRepresentation &representation,
               EvaluationTime &last_updated, void *value, std::uint32_t item, OutputPath *parent) noexcept
        : owner{parent == nullptr ? reinterpret_cast<std::uintptr_t>(&context) | context_tag
                                  : reinterpret_cast<std::uintptr_t>(parent)},
          representation{&representation}, last_updated{&last_updated}, value{value}, item{item} {}

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
    const TimeSeriesStorageRepresentation *representation;
    EvaluationTime *last_updated;
    void *value;
    std::uint32_t item;
};

static_assert(sizeof(OutputPath) == 40);
static_assert(alignof(EvaluationContext) >= 2);

} // namespace

void *add_output_path(EvaluationContext &context, const TimeSeriesStorageRepresentation &representation,
                      EvaluationTime &last_updated, void *value, std::size_t item, void *parent) {
    auto *storage = context.scratch().allocate(sizeof(OutputPath), alignof(OutputPath));
    return std::construct_at(static_cast<OutputPath *>(storage), context, representation, last_updated, value,
                             static_cast<std::uint32_t>(item), static_cast<OutputPath *>(parent));
}

EvaluationContext &output_path_context(void *path) noexcept {
    return static_cast<OutputPath *>(path)->context();
}

void publish_output_path(void *output_path) noexcept {
    auto *path = static_cast<OutputPath *>(output_path);
    while (path != nullptr) {
        auto &entry = *path;
        auto &context = entry.context();
        const auto modified = entry.representation->mark_item_modified(
            entry.value, entry.item, context.evaluation_time(), *entry.last_updated, current_scalar_memory_resource());
        *entry.last_updated = entry.representation->collection_valid(entry.value) ? context.evaluation_time() : MIN_DT;
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

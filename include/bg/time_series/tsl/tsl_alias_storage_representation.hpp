#pragma once

#include <bg/time_series/tsl/tsl_alias_input_view.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>

#include <memory>
#include <mutex>
#include <vector>

namespace bg::details {

template <std::size_t Size>
class AliasedFixedTimeSeriesListStorageRepresentation final : public TimeSeriesStorageRepresentation {
    using Storage = aliased_fixed_time_series_list_storage<Size>;

  public:
    explicit AliasedFixedTimeSeriesListStorageRepresentation(const TimeSeriesListType &type) : type_{&type} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(Storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(Storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return false; }
    [[nodiscard]] bool requires_finalization() const noexcept override { return false; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<AliasedFixedTimeSeriesListInputView<Size>>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override { return nullptr; }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        const auto &storage = *static_cast<const Storage *>(value);
        return aliased_list_valid(storage);
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const Storage *>(value);
        if (storage.node == nullptr) {
            return;
        }

        const auto *runtime_storage = aliased_list_runtime_storage(storage);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto &binding = storage.node->parameters()[index];
            const auto &representation = aliased_list_item_representation(storage, binding, index);
            function(context, {index, &representation, &item_last_updated, binding.value(runtime_storage)});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < Size; ++index) {
                publish(index, MIN_DT);
            }
            return;
        }
        if (storage.changes.time() != evaluation_time) {
            return;
        }

        for (const auto index : storage.changes) {
            const auto &binding = storage.node->parameters()[index];
            publish(index, binding.last_updated(runtime_storage));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &) const noexcept override {
        std::destroy_at(static_cast<Storage *>(value));
    }

  private:
    const TimeSeriesListType *type_;
};

template <std::size_t Size>
[[nodiscard]] const TimeSeriesStorageRepresentation &
aliased_fixed_time_series_list_storage_representation(const TimeSeriesListType &type) {
    using Representation = AliasedFixedTimeSeriesListStorageRepresentation<Size>;
    static auto mutex = std::mutex{};
    static auto representations = std::vector<std::unique_ptr<Representation>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing =
        std::ranges::find_if(representations, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != representations.end()) {
        return **existing;
    }

    representations.push_back(std::make_unique<Representation>(type));
    return *representations.back();
}

} // namespace bg::details

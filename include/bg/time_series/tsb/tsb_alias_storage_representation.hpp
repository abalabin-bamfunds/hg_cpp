#pragma once

#include <bg/time_series/tsb/tsb_alias_input_view.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

class AliasedTimeSeriesBundleStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    explicit AliasedTimeSeriesBundleStorageRepresentation(const TimeSeriesBundleType &type) : type_{&type} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override {
        return sizeof(aliased_time_series_bundle_storage);
    }
    [[nodiscard]] std::size_t value_alignment() const noexcept override {
        return alignof(aliased_time_series_bundle_storage);
    }
    [[nodiscard]] bool uses_variable_memory() const noexcept override {
        return type_->item_count() > util::index<>::inline_capacity;
    }
    [[nodiscard]] bool requires_finalization() const noexcept override { return false; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<AliasedTimeSeriesBundleInputView>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override { return nullptr; }

    [[nodiscard]] bool collection_valid(const void *value) const noexcept override {
        return aliased_bundle_valid(*static_cast<const aliased_time_series_bundle_storage *>(value));
    }

    void for_each_modified_item(const EvaluationTime &last_updated, const void *value, EvaluationTime evaluation_time,
                                void *context, ModifiedItemFunction function) const override {
        const auto &storage = *static_cast<const aliased_time_series_bundle_storage *>(value);
        if (storage.node == nullptr) {
            return;
        }
        const auto *runtime_storage = aliased_bundle_runtime_storage(storage);
        const auto publish = [&](std::size_t index, const EvaluationTime &item_last_updated) {
            const auto &binding = storage.node->parameters()[index];
            const auto &representation = aliased_bundle_item_representation(storage, binding, index);
            function(context, {index, &representation, &item_last_updated, binding.value(runtime_storage)});
        };

        if (last_updated <= MIN_DT) {
            for (auto index = std::size_t{}; index < storage.node->parameters().size(); ++index) {
                publish(index, MIN_DT);
            }
            return;
        }
        if (storage.change_time != evaluation_time) {
            return;
        }
        for (const auto index : storage.modified) {
            const auto &binding = storage.node->parameters()[index];
            publish(index, binding.last_updated(runtime_storage));
        }
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<aliased_time_series_bundle_storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<aliased_time_series_bundle_storage *>(value);
        storage->modified.release(memory);
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesBundleType *type_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
aliased_time_series_bundle_storage_representation(const TimeSeriesBundleType &type) {
    static auto mutex = std::mutex{};
    static auto representations = std::vector<std::unique_ptr<AliasedTimeSeriesBundleStorageRepresentation>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing =
        std::ranges::find_if(representations, [&](const auto &candidate) { return &candidate->type() == &type; });
    if (existing != representations.end()) {
        return **existing;
    }
    representations.push_back(std::make_unique<AliasedTimeSeriesBundleStorageRepresentation>(type));
    return *representations.back();
}

} // namespace bg::details

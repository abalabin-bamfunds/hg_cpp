#pragma once

#include <bg/time_series/cpp_storage_representation.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/time_series/tss/tss_input_view.hpp>
#include <bg/time_series/tss/tss_output_view.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {

class TimeSeriesSetStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    TimeSeriesSetStorageRepresentation(const TimeSeriesSetType &type,
                                       const ScalarStorageRepresentation &key) noexcept
        : type_{&type}, key_{&key},
          input_{util::polymorphic_value<TimeSeriesInputView>::copy(RuntimeTimeSeriesSetInputView{*this})},
          output_{util::polymorphic_value<TimeSeriesOutputView>::copy(RuntimeTimeSeriesSetOutputView{*this})} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(time_series_set_storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(time_series_set_storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_finalization() const noexcept override { return key_->requires_finalization(); }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }
    [[nodiscard]] const ScalarStorageRepresentation *key_storage_representation() const noexcept override {
        return key_;
    }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override { return input_.get(); }
    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        return &output_.get();
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_set_storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<time_series_set_storage *>(value);
        const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
        storage->release(*key_, layout, memory, [](std::uint32_t) noexcept {});
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesSetType *type_;
    const ScalarStorageRepresentation *key_;
    util::polymorphic_value<TimeSeriesInputView> input_;
    util::polymorphic_value<TimeSeriesOutputView> output_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
time_series_set_storage_representation(const TimeSeriesSetType &type,
                                       const ScalarStorageRepresentation &key) {
    struct Entry {
        const TimeSeriesSetType *type;
        const ScalarStorageRepresentation *key;
        std::unique_ptr<TimeSeriesSetStorageRepresentation> representation;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.type == &type && entry.key == &key;
    });
    if (found != entries.end()) {
        return *found->representation;
    }

    auto representation = std::make_unique<TimeSeriesSetStorageRepresentation>(type, key);
    auto &result = *representation;
    entries.push_back({&type, &key, std::move(representation)});
    return result;
}

template <typename K, auto... Properties> struct CppTimeSeriesStorageRepresentation<TSS<K, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) {
        return time_series_set_storage_representation(static_cast<const TimeSeriesSetType &>(type),
                                                      cpp_scalar_storage_representation<K>());
    }
};

} // namespace bg::details

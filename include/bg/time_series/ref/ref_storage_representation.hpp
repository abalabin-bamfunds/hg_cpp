#pragma once

#include <bg/time_series/ref/ref_input_view.hpp>
#include <bg/time_series/ref/ref_output_view.hpp>
#include <bg/time_series/time_series_storage_representation.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg {

class TimeSeriesReferenceStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    TimeSeriesReferenceStorageRepresentation(const TimeSeriesReferenceType &type,
                                             const TimeSeriesStorageRepresentation &target) noexcept
        : type_{&type}, target_{&target} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(details::ReferenceStorage *); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(details::ReferenceStorage *); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return false; }
    [[nodiscard]] bool requires_finalization() const noexcept override { return false; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }
    [[nodiscard]] const TimeSeriesStorageRepresentation *reference_target_representation() const noexcept override {
        return target_;
    }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<TimeSeriesReferenceInputView>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView &output_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesOutputView>::template make<TimeSeriesReferenceOutputView>();
        return prototype.get();
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<details::ReferenceStorage **>(destination), nullptr);
    }

    void destroy_value(void *, std::pmr::memory_resource &) const noexcept override {}

  private:
    const TimeSeriesReferenceType *type_;
    const TimeSeriesStorageRepresentation *target_;
};

[[nodiscard]] inline const TimeSeriesStorageRepresentation &
time_series_reference_storage_representation(const TimeSeriesReferenceType &type,
                                             const TimeSeriesStorageRepresentation &target) {
    struct Key {
        const TimeSeriesReferenceType *type;
        const TimeSeriesStorageRepresentation *target;

        bool operator==(const Key &) const = default;
    };
    struct Hash {
        [[nodiscard]] std::size_t operator()(const Key &key) const noexcept {
            const auto type_hash = std::hash<const TimeSeriesReferenceType *>{}(key.type);
            return type_hash ^ (std::hash<const TimeSeriesStorageRepresentation *>{}(key.target) << 1U);
        }
    };

    static auto mutex = std::mutex{};
    static auto representations =
        std::unordered_map<Key, std::unique_ptr<TimeSeriesReferenceStorageRepresentation>, Hash>{};

    const auto lock = std::lock_guard{mutex};
    auto [entry, inserted] = representations.try_emplace(Key{&type, &target});
    if (inserted) {
        entry->second = std::make_unique<TimeSeriesReferenceStorageRepresentation>(type, target);
    }
    return *entry->second;
}

} // namespace bg

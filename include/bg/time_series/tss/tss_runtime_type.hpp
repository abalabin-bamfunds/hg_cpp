#pragma once

#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/type/time_series_set_type.hpp>

#include <memory>

namespace bg::details {

class TimeSeriesSetRuntimeType;

struct RetainedSetKey {
    const TimeSeriesSetRuntimeType *runtime_type{};
    void *value{};
    item_id_t id{util::occupancy_index::npos};
};

class TimeSeriesSetRuntimeType : public TimeSeriesRuntimeType {
  public:
    TimeSeriesSetRuntimeType(const TimeSeriesSetType &type, const ScalarRuntimeType &key) noexcept
        : TimeSeriesSetRuntimeType{type, key, sizeof(time_series_set_storage), alignof(time_series_set_storage)} {}

    enum class Range : std::uint8_t { current, added, removed };

    bool is_reference_sample() const noexcept { return reference_sample_; }
    virtual std::size_t set_size(const void *value) const;
    virtual bool set_contains(const void *value, const ScalarData &key) const;
    virtual item_id_t find_set_item(const void *value, const ScalarData &key) const;
    virtual item_id_t find_set_key(const void *value, const ScalarData &key) const;
    virtual bool set_was_added(const TimeSeriesState &last_updated, const void *value, const ScalarData &key) const;
    virtual bool set_was_removed(const TimeSeriesState &last_updated, const void *value,
                                 const ScalarData &key) const;
    virtual const void *set_key(const void *value, item_id_t id) const noexcept;
    // The returned runtime type describes the key bytes and may differ from key_runtime_type() for sampled sets.
    virtual ScalarData set_key_data(const void *value, item_id_t id) const noexcept {
        return {key_runtime_type(), set_key(value, id)};
    }
    virtual item_id_t set_next_item_id(const TimeSeriesState &last_updated, const void *value, Range range,
                                       item_id_t start) const;
    virtual bool has_item_id_remapping(const void *) const { return false; }
    virtual void for_each_item_id_remapping(const void *, void *, ItemIdRemappingFunction) const {}

    const ScalarRuntimeType &key_runtime_type() const noexcept { return *key_; }

    ReferenceSampleFactory reference_sample_factory() const noexcept override;

    virtual RetainedSetKey retain_key(void *value, const ScalarData &key, allocator &memory) const;
    virtual void release_key(void *value, item_id_t id, allocator &memory) const noexcept;

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<time_series_set_storage *>(destination));
    }

    bool all_valid(const TimeSeriesState &last_updated, const void *) const noexcept override {
        return last_updated.valid();
    }

    void invalidate_output(const OutputState &output) const override;
    void copy_tick(const TimeSeriesInputData &input, OutputState &output) const override;
    void copy(const TimeSeriesInputData &input, OutputState &output) const override;

    void destroy_value(void *value, allocator &memory) const noexcept override {
        auto *storage = static_cast<time_series_set_storage *>(value);
        const auto layout = util::runtime_item_layout{key_->size(), key_->alignment()};
        storage->release(*key_, layout, memory, [](std::uint32_t) noexcept {});
        std::destroy_at(storage);
    }

  protected:
    TimeSeriesSetRuntimeType(const TimeSeriesSetType &type, const ScalarRuntimeType &key, std::size_t value_size,
                             std::size_t value_alignment, bool supports_byte_copy_initialization = true,
                             bool reference_sample = false) noexcept
        : TimeSeriesRuntimeType{type, value_size, value_alignment, true, key.requires_finalization(),
                                supports_byte_copy_initialization},
          key_{&key}, reference_sample_{reference_sample} {}

  private:
    const ScalarRuntimeType *key_;
    bool reference_sample_;
};

const TimeSeriesRuntimeType &time_series_set_runtime_type(const TimeSeriesSetType &type,
                                                          const ScalarRuntimeType &key);

template <typename K, auto... Properties> struct CppTimeSeriesRuntimeType<TSS<K, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return time_series_set_runtime_type(static_cast<const TimeSeriesSetType &>(type), cpp_scalar_runtime_type<K>());
    }
};

} // namespace bg::details

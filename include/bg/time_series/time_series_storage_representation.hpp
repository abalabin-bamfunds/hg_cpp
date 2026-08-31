#pragma once

#include <bg/evaluation_time.hpp>

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarStorageRepresentation;
class TimeSeriesInputView;
class TimeSeriesOutputView;
class TimeSeriesType;

class TimeSeriesStorageRepresentation {
  public:
    struct FixedItem {
        const TimeSeriesStorageRepresentation &representation;
        std::size_t last_updated_offset;
        std::size_t value_offset;
    };

    struct ModifiedItem {
        std::size_t item;
        const TimeSeriesStorageRepresentation *representation;
        const EvaluationTime *last_updated;
        const void *value;
    };

    using ModifiedItemFunction = void (*)(void *, ModifiedItem);

    virtual ~TimeSeriesStorageRepresentation() = default;

    [[nodiscard]] virtual const TimeSeriesType &type() const noexcept = 0;
    [[nodiscard]] virtual std::size_t value_size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t value_alignment() const noexcept = 0;
    [[nodiscard]] virtual bool uses_variable_memory() const noexcept = 0;
    [[nodiscard]] virtual bool requires_destruction() const noexcept = 0;
    [[nodiscard]] virtual bool supports_byte_copy_initialization() const noexcept = 0;
    [[nodiscard]] virtual const ScalarStorageRepresentation *scalar_storage_representation() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const TimeSeriesStorageRepresentation *reference_target_representation() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const TimeSeriesInputView &input_view_prototype() const noexcept = 0;
    [[nodiscard]] virtual const TimeSeriesOutputView &output_view_prototype() const noexcept = 0;
    [[nodiscard]] virtual FixedItem fixed_item(std::size_t) const;
    [[nodiscard]] virtual bool mark_item_modified(void *, std::size_t, EvaluationTime, EvaluationTime,
                                                  std::pmr::memory_resource &) const noexcept {
        return false;
    }
    [[nodiscard]] virtual bool collection_valid(const void *) const noexcept { return false; }
    virtual void for_each_modified_item(const EvaluationTime &, const void *, EvaluationTime, void *,
                                        ModifiedItemFunction) const {}
    [[nodiscard]] virtual bool finalize_modified_items(EvaluationTime &, void *, EvaluationTime) const noexcept {
        return true;
    }

    virtual void construct_initial_value(void *destination) const noexcept = 0;
    virtual void construct_runtime_value(void *destination, std::pmr::memory_resource &) const {
        construct_initial_value(destination);
    }
    virtual void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept = 0;
};

[[nodiscard]] const TimeSeriesStorageRepresentation &
time_series_value_storage_representation(const TimeSeriesType &type,
                                         const ScalarStorageRepresentation &scalar_representation);

template <typename TimeSeries>
[[nodiscard]] const TimeSeriesStorageRepresentation &cpp_time_series_storage_representation() noexcept;

} // namespace bg

#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/runtime_type.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>
#include <memory_resource>
#include <span>

namespace bg {

class ScalarRuntimeType;
namespace details {
class OutputState;
class ReferenceProvider;
class ReferenceRepresentation;
struct ItemReference;
} // namespace details

class TimeSeriesRuntimeType : public RuntimeType {
  public:
    struct ItemLayout {
        ItemLayout(const TimeSeriesRuntimeType &runtime_type, std::size_t last_updated_offset,
                   std::size_t value_offset) noexcept
            : runtime_type{runtime_type}, last_updated_offset{static_cast<std::uint32_t>(last_updated_offset)},
              value_offset{static_cast<std::uint32_t>(value_offset)} {}

        const TimeSeriesRuntimeType &runtime_type;
        std::uint32_t last_updated_offset;
        std::uint32_t value_offset;
    };

    static_assert(sizeof(ItemLayout) == 16);

    struct ModifiedItem {
        item_id_t id;
        const TimeSeriesRuntimeType *runtime_type;
        const EvaluationTime *last_updated;
        const void *value;
    };

    struct ConstItem {
        const TimeSeriesRuntimeType &runtime_type;
        const EvaluationTime &last_updated;
        const void *value;
    };

    struct MutableItem {
        const TimeSeriesRuntimeType &runtime_type;
        EvaluationTime &last_updated;
        void *value;
    };

    using ItemIdFunction = void (*)(void *, item_id_t);
    using ModifiedItemFunction = void (*)(void *, ModifiedItem);

    ~TimeSeriesRuntimeType() override;

    const TimeSeriesType &type() const noexcept {
        return static_cast<const TimeSeriesType &>(RuntimeType::type());
    }
    std::size_t value_size() const noexcept { return storage_size(); }
    std::size_t value_alignment() const noexcept { return storage_alignment(); }
    bool uses_variable_memory() const noexcept { return storage_uses_variable_memory(); }
    bool requires_finalization() const noexcept { return storage_requires_finalization(); }
    bool requires_destruction() const noexcept {
        return uses_variable_memory() || requires_finalization();
    }
    bool supports_byte_copy_initialization() const noexcept {
        return storage_supports_byte_copy_initialization();
    }
    virtual ItemLayout item_layout(item_id_t) const;
    virtual std::size_t item_count(const void *) const;
    virtual ConstItem item(const void *, item_id_t) const;
    virtual MutableItem item(void *, item_id_t) const;
    virtual void for_each_item_id(const void *, void *, ItemIdFunction) const;
    virtual bool item_updated(const EvaluationTime &, const void *, item_id_t) const noexcept;
    virtual void for_each_updated_item(const EvaluationTime &, const void *, void *, ItemIdFunction) const;
    [[nodiscard]] virtual bool mark_item_modified(void *, item_id_t, EvaluationTime, EvaluationTime,
                                                  std::pmr::memory_resource &) const noexcept {
        return false;
    }
    virtual bool collection_valid(const void *) const noexcept { return false; }
    virtual void for_each_modified_item(const EvaluationTime &, const void *, EvaluationTime, void *,
                                        ModifiedItemFunction) const {}
    virtual bool satisfies_reference_request(const TimeSeriesType &requested_type) const noexcept {
        return &type() == &requested_type;
    }
    virtual Reference reference(const void *) const;
    [[nodiscard]] virtual details::ReferenceRepresentation *
    create_reference_representation(details::ReferenceProvider &, Reference, const TimeSeriesType &,
                                    std::pmr::memory_resource &) const;
    virtual void construct_initial_value(void *destination) const noexcept = 0;
    virtual void construct_runtime_value(void *destination, std::pmr::memory_resource &) const {
        construct_initial_value(destination);
    }
    virtual void invalidate_output(const details::OutputState &output) const;
    virtual void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept = 0;

  protected:
    TimeSeriesRuntimeType(const TimeSeriesType &type, std::size_t value_size, std::size_t value_alignment,
                          bool uses_variable_memory, bool requires_finalization,
                          bool supports_byte_copy_initialization) noexcept
        : RuntimeType{type,
                      value_size,
                      value_alignment,
                      uses_variable_memory,
                      requires_finalization,
                      supports_byte_copy_initialization} {}

    explicit TimeSeriesRuntimeType(const TimeSeriesType &type) noexcept
        : TimeSeriesRuntimeType{type, 0, 1, false, false, false} {}

    static void invalidate_output_value(const details::OutputState &output) noexcept;
    static void invalidate_output_items(const details::OutputState &output);
};

class TimeSeriesValueRuntimeType : public TimeSeriesRuntimeType {
  public:
    const ScalarRuntimeType &scalar_runtime_type() const noexcept { return *scalar_; }

  protected:
    TimeSeriesValueRuntimeType(const TimeSeriesType &type, const ScalarRuntimeType &scalar, std::size_t value_size,
                               std::size_t value_alignment, bool uses_variable_memory, bool requires_finalization,
                               bool supports_byte_copy_initialization) noexcept
        : TimeSeriesRuntimeType{type,
                                value_size,
                                value_alignment,
                                uses_variable_memory,
                                requires_finalization,
                                supports_byte_copy_initialization},
          scalar_{&scalar} {}

  private:
    const ScalarRuntimeType *scalar_;
};

const TimeSeriesRuntimeType &time_series_value_runtime_type(const TimeSeriesType &type,
                                                                          const ScalarRuntimeType &scalar_runtime_type);

[[nodiscard]] const TimeSeriesRuntimeType &
resolve_time_series_runtime_type(const TimeSeriesType &type,
                                 std::span<const TimeSeriesRuntimeType *const> available_time_series = {},
                                 std::span<const ScalarRuntimeType *const> available_scalars = {});

template <typename TimeSeries> const TimeSeriesRuntimeType &cpp_time_series_runtime_type();

} // namespace bg

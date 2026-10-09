#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/runtime_type.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_state.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>
#include <span>

namespace bg {

class ScalarRuntimeType;
class OutputState;
class TimeSeriesInputData;
namespace details {
class ReferenceProvider;
class ReferenceRepresentation;
class ReferenceRepresentationFactory;
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
        const TimeSeriesState *last_updated;
        const void *value;
    };

    struct ConstItem {
        const TimeSeriesRuntimeType &runtime_type;
        const TimeSeriesState &last_updated;
        const void *value;
    };

    struct MutableItem {
        const TimeSeriesRuntimeType &runtime_type;
        TimeSeriesState &last_updated;
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
    bool is_reference() const noexcept { return has_runtime_type_flag(reference_flag); }
    virtual ItemLayout item_layout(item_id_t) const;
    virtual std::size_t item_count(const void *) const;
    virtual ConstItem item(const void *, item_id_t) const;
    virtual MutableItem item(void *, item_id_t) const;
    virtual void for_each_item_id(const void *, void *, ItemIdFunction) const;
    virtual bool item_updated(const TimeSeriesState &, const void *, item_id_t) const;
    virtual void for_each_updated_item(const TimeSeriesState &, const void *, void *, ItemIdFunction) const;
    // Storage overloads inspect physical data; input overloads also apply input-view guarantees.
    virtual bool all_valid(const TimeSeriesState &, const void *) const noexcept;
    virtual bool all_valid(const TimeSeriesInputData &) const noexcept;
    // The declared type must accept this runtime type; a bundle declaration limits the fields checked.
    virtual bool all_valid(const TimeSeriesState &, const void *, const TimeSeriesType &declared_type) const noexcept;
    virtual bool all_valid(const TimeSeriesInputData &, const TimeSeriesType &declared_type) const noexcept;
    [[nodiscard]] virtual bool mark_item_modified(void *, item_id_t, EvaluationTime, const TimeSeriesState &,
                                                  allocator &) const noexcept {
        return false;
    }
    virtual bool collection_valid(const void *) const noexcept { return false; }
    virtual void for_each_modified_item(const TimeSeriesState &, const void *, EvaluationTime, void *,
                                        ModifiedItemFunction) const {}
    virtual Reference reference(const void *) const;
    virtual void copy_tick(const TimeSeriesInputData &input, OutputState &output) const;
    virtual void copy(const TimeSeriesInputData &input, OutputState &output) const;
    virtual const details::ReferenceRepresentationFactory *reference_representation_factory() const noexcept {
        return nullptr;
    }
    virtual void construct_initial_value(void *destination) const noexcept = 0;
    virtual void construct_runtime_value(void *destination, allocator &) const {
        construct_initial_value(destination);
    }
    virtual void invalidate_output(const OutputState &output) const;
    virtual void destroy_value(void *value, allocator &memory) const noexcept = 0;

  protected:
    TimeSeriesRuntimeType(const TimeSeriesType &type, std::size_t value_size, std::size_t value_alignment,
                          bool uses_variable_memory, bool requires_finalization, bool supports_byte_copy_initialization,
                          bool is_reference = false) noexcept
        : RuntimeType{type,
                      value_size,
                      value_alignment,
                      uses_variable_memory,
                      requires_finalization,
                      supports_byte_copy_initialization,
                      is_reference ? reference_flag : std::uint8_t{}} {}

    explicit TimeSeriesRuntimeType(const TimeSeriesType &type) noexcept
        : TimeSeriesRuntimeType{type, 0, 1, false, false, false} {}

    static void invalidate_output_value(const OutputState &output) noexcept;
    static void invalidate_output_items(const OutputState &output);

  private:
    static constexpr auto reference_flag = first_additional_flag;
};

static_assert(sizeof(TimeSeriesRuntimeType) == sizeof(RuntimeType));

class TimeSeriesValueRuntimeType : public TimeSeriesRuntimeType {
  public:
    const ScalarRuntimeType &scalar_runtime_type() const noexcept { return *scalar_; }
    void copy(const TimeSeriesInputData &input, OutputState &output) const override;

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

// Registers the default physical representation for its semantic type. Explicitly supplied representations take
// precedence during resolution.
void register_time_series_runtime_type(const TimeSeriesRuntimeType &runtime_type);

template <typename TimeSeries> const TimeSeriesRuntimeType &cpp_time_series_runtime_type();

} // namespace bg

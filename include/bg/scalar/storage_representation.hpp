#pragma once

#include <cstddef>
#include <memory_resource>
#include <span>

namespace bg {

class ScalarType;
class ScalarValueInputView;
class ScalarListMutableInputView;
class ScalarSetMutableInputView;
class ScalarDictMutableInputView;
class TimeSeriesValueInputView;
class TimeSeriesValueOutputView;

class ScalarStorageRepresentation {
  public:
    virtual ~ScalarStorageRepresentation() = default;

    [[nodiscard]] virtual const ScalarType &type() const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t alignment() const noexcept = 0;
    [[nodiscard]] virtual bool uses_variable_memory() const noexcept = 0;
    [[nodiscard]] virtual bool requires_finalization() const noexcept = 0;
    [[nodiscard]] bool requires_destruction() const noexcept {
        return uses_variable_memory() || requires_finalization();
    }
    [[nodiscard]] virtual bool supports_byte_copy_initialization() const noexcept = 0;

    // A storage-only composed representation has no standalone view prototype.
    [[nodiscard]] virtual const ScalarValueInputView *scalar_input_view_prototype() const noexcept { return nullptr; }
    [[nodiscard]] virtual const TimeSeriesValueInputView *input_view_prototype() const noexcept { return nullptr; }
    [[nodiscard]] virtual const TimeSeriesValueOutputView *output_view_prototype() const noexcept { return nullptr; }
    [[nodiscard]] virtual const ScalarListMutableInputView *list_mutable_view_prototype() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const ScalarSetMutableInputView *set_mutable_view_prototype() const noexcept {
        return nullptr;
    }
    [[nodiscard]] virtual const ScalarDictMutableInputView *dict_mutable_view_prototype() const noexcept {
        return nullptr;
    }

    virtual void construct_initial_value(void *destination) const noexcept = 0;

    virtual void construct_runtime_value(void *destination, std::pmr::memory_resource &) const {
        construct_initial_value(destination);
    }

    virtual void construct_from_value(void *destination, const void *value,
                                      std::pmr::memory_resource &memory) const = 0;

    virtual void construct_from_input(void *destination, const ScalarValueInputView &value,
                                      std::pmr::memory_resource &memory) const = 0;

    virtual void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const = 0;

    virtual void assign_from_value(void *destination, const void *value) const = 0;

    virtual void assign_from_input(void *destination, const ScalarValueInputView &value) const = 0;

    [[nodiscard]] virtual std::size_t hash(const void *value) const noexcept = 0;
    [[nodiscard]] virtual bool equals(const void *value, const ScalarValueInputView &other) const noexcept = 0;

    virtual void destroy(void *value, std::pmr::memory_resource &memory) const noexcept = 0;
};

[[nodiscard]] const ScalarStorageRepresentation &
resolve_scalar_storage_representation(const ScalarType &type,
                                      std::span<const ScalarStorageRepresentation *const> available);

void register_scalar_storage_representation(const ScalarStorageRepresentation &representation);

template <typename T> [[nodiscard]] const ScalarStorageRepresentation &cpp_scalar_storage_representation();

} // namespace bg

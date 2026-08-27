#pragma once

#include <cstddef>
#include <memory_resource>

namespace bg {

class ScalarType;
class ScalarValueInputView;
class TimeSeriesValueInputView;
class TimeSeriesValueOutputView;

class ScalarStorageRepresentation {
  public:
    virtual ~ScalarStorageRepresentation() = default;

    [[nodiscard]] virtual const ScalarType &type() const noexcept = 0;
    [[nodiscard]] virtual std::size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::size_t alignment() const noexcept = 0;
    [[nodiscard]] virtual bool uses_variable_memory() const noexcept = 0;
    [[nodiscard]] virtual bool requires_destruction() const noexcept = 0;
    [[nodiscard]] virtual bool supports_byte_copy_initialization() const noexcept = 0;
    [[nodiscard]] virtual const ScalarValueInputView &scalar_input_view_prototype() const noexcept = 0;
    [[nodiscard]] virtual const TimeSeriesValueInputView &input_view_prototype() const noexcept = 0;
    [[nodiscard]] virtual const TimeSeriesValueOutputView &output_view_prototype() const noexcept = 0;

    virtual void construct_initial_value(void *destination) const noexcept = 0;

    virtual void construct_from_value(void *destination, const void *value,
                                      std::pmr::memory_resource &memory) const = 0;

    virtual void copy_construct(void *destination, const void *source, std::pmr::memory_resource &memory) const = 0;

    virtual void assign_from_value(void *destination, const void *value) const = 0;

    virtual void destroy(void *value, std::pmr::memory_resource &memory) const noexcept = 0;
};

template <typename T> [[nodiscard]] const ScalarStorageRepresentation &cpp_scalar_storage_representation() noexcept;

} // namespace bg

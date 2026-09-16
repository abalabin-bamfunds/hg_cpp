#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace bg {

class Type;

class RuntimeType {
  public:
    virtual ~RuntimeType();

    const Type &type() const noexcept { return *type_; }

  protected:
    RuntimeType(const Type &type, std::size_t size, std::size_t alignment, bool uses_variable_memory,
                bool requires_finalization, bool supports_byte_copy_initialization,
                std::uint8_t additional_flags = 0) noexcept
        : type_{&type}, size_{static_cast<std::uint32_t>(size)},
          alignment_log2_{static_cast<std::uint8_t>(std::countr_zero(alignment))},
          flags_{static_cast<std::uint8_t>(
              (uses_variable_memory ? variable_memory_flag : 0U) | (requires_finalization ? finalization_flag : 0U) |
              (supports_byte_copy_initialization ? byte_copy_flag : 0U) | additional_flags)} {}

    std::size_t storage_size() const noexcept { return size_; }
    std::size_t storage_alignment() const noexcept { return std::size_t{1} << alignment_log2_; }
    bool storage_uses_variable_memory() const noexcept { return flags_ & variable_memory_flag; }
    bool storage_requires_finalization() const noexcept { return flags_ & finalization_flag; }
    bool storage_supports_byte_copy_initialization() const noexcept { return flags_ & byte_copy_flag; }
    bool has_runtime_type_flag(std::uint8_t flag) const noexcept { return (flags_ & flag) != 0; }
    static constexpr auto first_additional_flag = std::uint8_t{1U << 3U};

    void set_storage_layout(std::size_t size, std::size_t alignment, bool uses_variable_memory,
                            bool requires_finalization, bool supports_byte_copy_initialization) noexcept {
        size_ = static_cast<std::uint32_t>(size);
        alignment_log2_ = static_cast<std::uint8_t>(std::countr_zero(alignment));
        flags_ =
            static_cast<std::uint8_t>((flags_ & ~storage_flags) | (uses_variable_memory ? variable_memory_flag : 0U) |
                                      (requires_finalization ? finalization_flag : 0U) |
                                      (supports_byte_copy_initialization ? byte_copy_flag : 0U));
    }

  private:
    static constexpr auto variable_memory_flag = std::uint8_t{1U << 0U};
    static constexpr auto finalization_flag = std::uint8_t{1U << 1U};
    static constexpr auto byte_copy_flag = std::uint8_t{1U << 2U};
    static constexpr auto storage_flags = variable_memory_flag | finalization_flag | byte_copy_flag;

    const Type *type_;
    std::uint32_t size_;
    std::uint8_t alignment_log2_;
    std::uint8_t flags_;
};

} // namespace bg

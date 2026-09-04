#pragma once

#include <bg/scalar/details/value_type.hpp>
#include <bg/scalar/runtime_type.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <utility>

namespace bg::details {

enum class ScalarDataMode : std::uintptr_t {
    stored_view,
    source_view,
    inline_value,
    owned_explicit_resource,
    owned_context_resource,
    owned_standard_resource,
};

inline constexpr auto scalar_data_mode_mask = std::uintptr_t{7};
static_assert(std::to_underlying(ScalarDataMode::owned_standard_resource) <= scalar_data_mode_mask);

class ScalarData {
  public:
    ScalarData() = default;

    ScalarData(const ScalarRuntimeType &runtime_type, const void *data,
               ScalarDataMode mode = ScalarDataMode::stored_view) noexcept
        : ScalarData{runtime_type, reinterpret_cast<std::uintptr_t>(data), mode} {}

    ScalarData(const ScalarRuntimeType &runtime_type, std::uintptr_t data,
               ScalarDataMode mode = ScalarDataMode::stored_view) noexcept
        : runtime_type_{tag(runtime_type, mode)}, data_{data} {}

    [[nodiscard]] const ScalarRuntimeType &runtime_type() const noexcept {
        return *reinterpret_cast<const ScalarRuntimeType *>(reinterpret_cast<std::uintptr_t>(runtime_type_) &
                                                            ~scalar_data_mode_mask);
    }

    [[nodiscard]] ScalarDataMode mode() const noexcept {
        return static_cast<ScalarDataMode>(reinterpret_cast<std::uintptr_t>(runtime_type_) & scalar_data_mode_mask);
    }
    [[nodiscard]] const void *data() const noexcept {
        return mode() == ScalarDataMode::inline_value ? std::addressof(data_) : reinterpret_cast<const void *>(data_);
    }
    [[nodiscard]] const ScalarType &type() const noexcept { return runtime_type().type(); }
    [[nodiscard]] bool empty() const noexcept { return runtime_type_ == nullptr; }
    [[nodiscard]] std::uintptr_t data_word() const noexcept { return data_; }

    [[nodiscard]] std::size_t hash() const noexcept { return runtime_type().hash(mode(), data_); }
    [[nodiscard]] bool equals(const ScalarData &other) const noexcept {
        return runtime_type().equals(mode(), data_, other);
    }

    void copy_construct(void *destination, const ScalarRuntimeType &destination_runtime_type,
                        std::pmr::memory_resource &memory) const {
        if (&type() != &destination_runtime_type.type()) {
            throw std::invalid_argument{"incompatible scalar types"};
        }
        destination_runtime_type.construct_from_input(destination, *this, memory);
    }

    void assign(void *destination, const ScalarRuntimeType &destination_runtime_type) const {
        if (&type() != &destination_runtime_type.type()) {
            throw std::invalid_argument{"incompatible scalar types"};
        }
        destination_runtime_type.assign_from_input(destination, *this);
    }

  private:
    [[nodiscard]] static const ScalarRuntimeType *tag(const ScalarRuntimeType &runtime_type,
                                                      ScalarDataMode mode) noexcept {
        const auto pointer = reinterpret_cast<std::uintptr_t>(std::addressof(runtime_type));
        return reinterpret_cast<const ScalarRuntimeType *>(pointer | std::to_underlying(mode));
    }

    const ScalarRuntimeType *runtime_type_{};
    std::uintptr_t data_{};
};

static_assert(sizeof(ScalarData) == 2 * sizeof(void *));

template <typename T> [[nodiscard]] scalar_value_t<T> scalar_data_value(const ScalarData &source);

} // namespace bg::details

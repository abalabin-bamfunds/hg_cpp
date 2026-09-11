#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/details/value_type.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace bg::details {

enum class ScalarDataMode : std::uintptr_t {
    stored_view,
    source_view,
    inline_value,
    owned_explicit_allocator,
    owned_context_allocator,
    owned_default_allocator,
};

inline constexpr auto scalar_data_mode_mask = std::uintptr_t{7};
static_assert(std::to_underlying(ScalarDataMode::owned_default_allocator) <= scalar_data_mode_mask);

class ScalarData {
  public:
    ScalarData() = default;

    ScalarData(const ScalarRuntimeType &runtime_type, const void *data,
               ScalarDataMode mode = ScalarDataMode::stored_view) noexcept
        : ScalarData{runtime_type, reinterpret_cast<std::uintptr_t>(data), mode} {}

    ScalarData(const ScalarRuntimeType &runtime_type, std::uintptr_t data,
               ScalarDataMode mode = ScalarDataMode::stored_view) noexcept
        : runtime_type_{&runtime_type, mode}, data_{data} {}

    const ScalarRuntimeType &runtime_type() const noexcept { return *runtime_type_; }

    ScalarDataMode mode() const noexcept { return runtime_type_.tag(); }
    const void *data() const noexcept {
        return mode() == ScalarDataMode::inline_value ? std::addressof(data_) : reinterpret_cast<const void *>(data_);
    }
    const ScalarType &type() const noexcept { return runtime_type().type(); }
    bool empty() const noexcept { return !runtime_type_; }
    std::uintptr_t data_word() const noexcept { return data_; }

    std::size_t hash() const noexcept { return runtime_type().hash(mode(), data_); }
    bool equals(const ScalarData &other) const noexcept {
        return runtime_type().equals(mode(), data_, other);
    }

    void copy_construct(void *destination, const ScalarRuntimeType &destination_runtime_type,
                        allocator &memory) const {
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
    util::tagged_ptr<const ScalarRuntimeType, 3, ScalarDataMode> runtime_type_;
    std::uintptr_t data_{};
};

static_assert(sizeof(ScalarData) == 2 * sizeof(void *));

template <typename T> scalar_value_t<T> scalar_data_value(const ScalarData &source);

} // namespace bg::details

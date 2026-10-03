#pragma once

#include <bg/allocator.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace bg {

// Describes the ownership and representation of a scalar value for extension and runtime implementers.
// Node definitions use Scalar and ScalarValue.
enum class ScalarDataMode : std::uintptr_t {
    stored_view,
    source_view,
    inline_value,
    owned_explicit_allocator,
    owned_context_allocator,
    owned_default_allocator,
    shared_view,
    shared_value,
};

static_assert(std::to_underlying(ScalarDataMode::shared_value) <= std::uintptr_t{7});

// Binds a scalar runtime representation to its storage. Extension and runtime implementers use this type to pass
// scalar values without copying them. Node definitions use Scalar and ScalarValue.
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
        const auto direct = mode() == ScalarDataMode::inline_value || mode() == ScalarDataMode::shared_view ||
                            mode() == ScalarDataMode::shared_value;
        return direct ? std::addressof(data_) : reinterpret_cast<const void *>(data_);
    }
    const ScalarType &type() const noexcept { return runtime_type().type(); }
    bool empty() const noexcept { return !runtime_type_; }
    std::uintptr_t data_word() const noexcept { return data_; }

    std::size_t hash() const noexcept { return runtime_type().hash(mode(), data_); }
    bool equals(const ScalarData &other) const noexcept { return runtime_type().equals(mode(), data_, other); }

    void copy_construct(void *destination, const ScalarRuntimeType &destination_runtime_type, allocator &memory) const {
        destination_runtime_type.construct_from_input(destination, *this, memory);
    }

    void assign(void *destination, const ScalarRuntimeType &destination_runtime_type) const {
        destination_runtime_type.assign_from_input(destination, *this);
    }

  private:
    util::tagged_ptr<const ScalarRuntimeType, 3, ScalarDataMode> runtime_type_;
    std::uintptr_t data_{};
};

static_assert(sizeof(ScalarData) == 2 * sizeof(void *));

} // namespace bg

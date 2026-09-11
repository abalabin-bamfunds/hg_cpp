#pragma once

#include <bg/util/scope_exit.hpp>

#include <concepts>
#include <cstddef>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

namespace bg {

// Allocates objects through a non-owning polymorphic memory resource.
class allocator {
  public:
    explicit allocator(std::pmr::memory_resource &memory) noexcept : memory_{&memory} {}

    [[nodiscard]] void *allocate_bytes(std::size_t size, std::size_t alignment) {
        return memory_->allocate(size, alignment);
    }

    void deallocate_bytes(void *storage, std::size_t size, std::size_t alignment) noexcept {
        memory_->deallocate(storage, size, alignment);
    }

    template <typename T, typename... Arguments>
        requires std::constructible_from<T, Arguments...>
    [[nodiscard]] T *new_object(Arguments &&...arguments) {
        auto *storage = static_cast<T *>(allocate_bytes(sizeof(T), alignof(T)));
        auto release = util::scope_exit{[&]() noexcept { deallocate_bytes(storage, sizeof(T), alignof(T)); }};
        auto *result = std::construct_at(storage, std::forward<Arguments>(arguments)...);
        release.release();
        return result;
    }

    template <typename T> void delete_object(T *object) noexcept {
        if (object == nullptr) {
            return;
        }

        std::destroy_at(object);
        deallocate_bytes(object, sizeof(T), alignof(T));
    }

    std::pmr::memory_resource &memory_resource() const noexcept { return *memory_; }

  private:
    std::pmr::memory_resource *memory_;
};

static_assert(sizeof(allocator) == sizeof(void *));
static_assert(std::is_trivially_copyable_v<allocator>);

} // namespace bg

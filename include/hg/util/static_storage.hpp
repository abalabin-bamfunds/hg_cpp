#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace bg::util {

// Stores a function-static object without running its destructor at process shutdown.
template <typename T> class static_storage {
  public:
    template <typename... Arguments>
    explicit static_storage(Arguments &&...arguments) {
        std::construct_at(reinterpret_cast<T *>(storage_), std::forward<Arguments>(arguments)...);
    }

    template <typename Factory>
    static_storage(std::in_place_t, Factory &&make_value) {
        ::new (static_cast<void *>(storage_)) T(std::forward<Factory>(make_value)());
    }

    static_storage(const static_storage &) = delete;
    static_storage &operator=(const static_storage &) = delete;

    T &get() noexcept { return *std::launder(reinterpret_cast<T *>(storage_)); }
    const T &get() const noexcept { return *std::launder(reinterpret_cast<const T *>(storage_)); }

  private:
    alignas(T) std::byte storage_[sizeof(T)];
};

static_assert(std::is_trivially_destructible_v<static_storage<int>>);

} // namespace bg::util

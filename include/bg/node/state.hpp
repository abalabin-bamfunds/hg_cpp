#pragma once

#include <type_traits>

namespace bg {

template <typename T> class State {
    static_assert(std::is_nothrow_default_constructible_v<T>);
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    explicit State(T &value) noexcept : value_(&value) {}

    [[nodiscard]] T *operator->() const noexcept { return value_; }
    [[nodiscard]] T &operator*() const noexcept { return *value_; }

    using value_type = T;

  private:
    T *value_;
};

static_assert(sizeof(State<int>) == sizeof(void *));

} // namespace bg

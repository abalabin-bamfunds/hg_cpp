#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

namespace bg::util {

template <typename Function>
    requires std::is_nothrow_invocable_v<Function &>
class [[nodiscard]] scope_exit {
  public:
    explicit scope_exit(Function function) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_{std::move(function)} {}

    scope_exit(scope_exit &&other) noexcept(std::is_nothrow_move_constructible_v<Function>)
        : function_{std::move(other.function_)}, active_{std::exchange(other.active_, false)} {}

    scope_exit(const scope_exit &) = delete;
    scope_exit &operator=(const scope_exit &) = delete;
    scope_exit &operator=(scope_exit &&) = delete;

    ~scope_exit() noexcept {
        if (active_) {
            std::invoke(function_);
        }
    }

    void release() noexcept { active_ = false; }

  private:
    [[no_unique_address]] Function function_;
    bool active_{true};
};

template <typename Function> scope_exit(Function) -> scope_exit<std::decay_t<Function>>;

} // namespace bg::util

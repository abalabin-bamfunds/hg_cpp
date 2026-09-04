#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/time_series/details/output_path.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace bg::details {

// Owns an output state in invocation scratch storage or borrows an existing state from another output view.
template <typename State> class output_proxy {
  public:
    [[nodiscard]] static output_proxy copy(const State &prototype, EvaluationContext &context) {
        auto *storage = context.scratch().allocate(sizeof(State), alignof(State));
        return output_proxy{*std::construct_at(static_cast<State *>(storage), prototype)};
    }

    [[nodiscard]] static output_proxy copy(const State &prototype, void *output_path) {
        return copy(prototype, output_path_context(output_path));
    }

    [[nodiscard]] static output_proxy borrow(State &state) noexcept {
        return output_proxy{reinterpret_cast<std::uintptr_t>(&state) | borrowed_tag};
    }

    output_proxy(const output_proxy &) = delete;
    output_proxy &operator=(const output_proxy &) = delete;

    output_proxy(output_proxy &&other) noexcept : state_{std::exchange(other.state_, 0)} {}
    output_proxy &operator=(output_proxy &&) = delete;

    ~output_proxy() {
        if (state_ != 0 && (state_ & borrowed_tag) == 0) {
            get().release();
        }
    }

    [[nodiscard]] State &get() const noexcept { return *pointer(); }
    [[nodiscard]] State *operator->() noexcept { return pointer(); }
    [[nodiscard]] State *operator->() const noexcept { return pointer(); }

  private:
    explicit output_proxy(State &state) noexcept : state_{reinterpret_cast<std::uintptr_t>(&state)} {}
    explicit output_proxy(std::uintptr_t state) noexcept : state_{state} {}

    [[nodiscard]] State *pointer() const noexcept { return reinterpret_cast<State *>(state_ & ~borrowed_tag); }

    static constexpr auto borrowed_tag = std::uintptr_t{1};

    std::uintptr_t state_;
};

} // namespace bg::details

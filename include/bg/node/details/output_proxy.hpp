#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/time_series/details/output_path.hpp>
#include <bg/util/tagged_ptr.hpp>

#include <cstdint>
#include <memory>

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

    [[nodiscard]] static output_proxy borrow(State &state) noexcept { return output_proxy{state, Ownership::borrowed}; }

    output_proxy(const output_proxy &) = delete;
    output_proxy &operator=(const output_proxy &) = delete;

    output_proxy(output_proxy &&other) noexcept : state_{other.state_} { other.state_.reset(); }
    output_proxy &operator=(output_proxy &&) = delete;

    ~output_proxy() {
        if (state_ && state_.tag() == Ownership::owned) {
            get().release();
        }
    }

    State &get() const noexcept { return *pointer(); }
    State *operator->() noexcept { return pointer(); }
    State *operator->() const noexcept { return pointer(); }

  private:
    enum class Ownership : std::uintptr_t {
        owned,
        borrowed,
    };

    explicit output_proxy(State &state, Ownership ownership = Ownership::owned) noexcept : state_{&state, ownership} {}

    State *pointer() const noexcept { return state_.get(); }

    util::tagged_ptr<State, 1, Ownership> state_;
};

} // namespace bg::details

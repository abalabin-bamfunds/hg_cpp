#pragma once

#include <atomic>
#include <cstdint>

namespace bg::details {

// Links a process-static object once without allocation; concurrent callers wait until publication completes.
template <typename T> class StaticRegistryEntry {
  public:
    explicit StaticRegistryEntry(const T &object) noexcept : object_{&object} {}

    void link(std::atomic<StaticRegistryEntry<T> *> &head) noexcept {
        auto expected = State::unlinked;
        if (!state_.compare_exchange_strong(expected, State::linking, std::memory_order_acquire,
                                            std::memory_order_acquire)) {
            while (state_.load(std::memory_order_acquire) != State::linked) {
            }
            return;
        }

        auto *entry = head.load(std::memory_order_relaxed);
        do {
            next_.store(entry, std::memory_order_relaxed);
        } while (!head.compare_exchange_weak(entry, this, std::memory_order_release, std::memory_order_relaxed));
        state_.store(State::linked, std::memory_order_release);
    }

    const T &object() const noexcept { return *object_; }
    StaticRegistryEntry<T> *next() const noexcept { return next_.load(std::memory_order_acquire); }

  private:
    enum class State : std::uint8_t { unlinked, linking, linked };

    const T *object_;
    std::atomic<StaticRegistryEntry<T> *> next_{};
    std::atomic<State> state_{};
};

} // namespace bg::details

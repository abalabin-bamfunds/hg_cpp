#pragma once

namespace bg::details {

// Links a process-static object once without allocation during type registration.
template <typename T> class StaticRegistryEntry {
  public:
    explicit StaticRegistryEntry(const T &object) noexcept : object_{&object} {}

    void link(StaticRegistryEntry<T> *&head) noexcept {
        if (linked_) {
            return;
        }

        next_ = head;
        head = this;
        linked_ = true;
    }

    const T &object() const noexcept { return *object_; }
    StaticRegistryEntry<T> *next() const noexcept { return next_; }

  private:
    const T *object_;
    StaticRegistryEntry<T> *next_{};
    bool linked_{};
};

} // namespace bg::details

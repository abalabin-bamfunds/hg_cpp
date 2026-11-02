#pragma once

#include <typeinfo>

namespace hg::details {

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

    bool link(StaticRegistryEntry<T> *&head, const std::type_info &declaration,
              const std::type_info &descriptor) noexcept {
        if (linked_) {
            return false;
        }

        declaration_ = &declaration;
        descriptor_ = &descriptor;
        link(head);
        return true;
    }

    bool matches(const std::type_info &declaration, const std::type_info &descriptor) const noexcept {
        return declaration_ != nullptr && *declaration_ == declaration && *descriptor_ == descriptor;
    }

    const T &object() const noexcept { return *object_; }
    StaticRegistryEntry<T> *next() const noexcept { return next_; }

  private:
    const T *object_;
    StaticRegistryEntry<T> *next_{};
    const std::type_info *declaration_{};
    const std::type_info *descriptor_{};
    bool linked_{};
};

} // namespace hg::details

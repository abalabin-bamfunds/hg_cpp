#pragma once

#include <cstdint>

namespace bg::util {

class thread_local_pointer {
  public:
    thread_local_pointer();
    ~thread_local_pointer();

    thread_local_pointer(const thread_local_pointer &) = delete;
    thread_local_pointer &operator=(const thread_local_pointer &) = delete;

    void *get() const noexcept;
    void set(void *value) noexcept;

  private:
    std::uintptr_t key_;
};

} // namespace bg::util

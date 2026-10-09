#pragma once

#include <bg/allocator.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>

namespace bg {

struct string;
class ScalarData;
template <typename T> class ScalarValue;
class StringBuilder;

namespace details {

class BundleAllocationOwner;

class StringStorage;

struct StringStorageFinalReleaseHook {
    using Callback = void (*)(void *context, const StringStorage *storage) noexcept;

    Callback callback;
    void *context;
};

// Shared immutable UTF-8 bytes used by the built-in string scalar.
class StringStorage {
  public:
    StringStorage(const StringStorage &) = delete;
    StringStorage &operator=(const StringStorage &) = delete;

    [[nodiscard]] static StringStorage *copy(std::string_view value, allocator &memory);
    [[nodiscard]] static StringStorage *copy(std::string_view value);
    [[nodiscard]] static StringStorage *from(const ScalarData &value) noexcept;

    std::string_view value() const noexcept;
    const char *data() const noexcept;
    std::size_t size() const noexcept { return length_; }
    std::size_t capacity() const noexcept { return capacity_; }

    // Installs a callback record that remains owned by its extension until final release.
    // Returns the installed record, or the record already installed by another caller.
    StringStorageFinalReleaseHook *install_final_release_hook(StringStorageFinalReleaseHook *hook) noexcept;
    StringStorageFinalReleaseHook *final_release_hook() const noexcept {
        return final_release_hook_.load(std::memory_order_acquire);
    }

    // Retains this storage only while another owner still keeps it alive.
    bool try_retain() noexcept;
    void retain() noexcept;
    void release() noexcept;

  private:
    friend class ::bg::StringBuilder;

    StringStorage(std::pmr::memory_resource &memory, BundleAllocationOwner *owner, std::size_t length,
                  std::size_t capacity) noexcept;

    [[nodiscard]] static StringStorage *create_builder(std::size_t capacity, allocator &memory);
    [[nodiscard]] static StringStorage *grow(StringStorage &source, std::size_t capacity);
    [[nodiscard]] static StringStorage *allocate(std::size_t length, std::size_t capacity, allocator &memory,
                                                 BundleAllocationOwner *owner);
    static void deallocate(StringStorage *storage) noexcept;
    char *mutable_data() noexcept;
    void set_size(std::size_t size);

    std::atomic_size_t references_{1};
    std::pmr::memory_resource *memory_;
    BundleAllocationOwner *owner_;
    std::uint32_t length_;
    std::uint32_t capacity_;
    std::atomic<StringStorageFinalReleaseHook *> final_release_hook_{};
};

} // namespace details
} // namespace bg

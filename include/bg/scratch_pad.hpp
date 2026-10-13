#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace bg {

namespace details {
class EvaluationScratch;
}

class EvaluationContext;
class RuntimeGraph;

// Provides temporary storage for one node invocation.
class ScratchPad {
  public:
    class LocalStorageScope {
      public:
        LocalStorageScope(const LocalStorageScope &) = delete;
        LocalStorageScope &operator=(const LocalStorageScope &) = delete;
        LocalStorageScope(LocalStorageScope &&) = delete;
        LocalStorageScope &operator=(LocalStorageScope &&) = delete;

        ~LocalStorageScope();

      private:
        friend ScratchPad;

        LocalStorageScope(ScratchPad &scratch_pad, std::span<std::byte> storage) noexcept;

        ScratchPad *scratch_pad_;
        LocalStorageScope *previous_;
        std::span<std::byte> storage_;
        std::size_t used_{};
    };

    ~ScratchPad();

    ScratchPad(const ScratchPad &) = delete;
    ScratchPad &operator=(const ScratchPad &) = delete;
    ScratchPad(ScratchPad &&other) noexcept;
    ScratchPad &operator=(ScratchPad &&) = delete;

    [[nodiscard]] void *allocate(std::size_t size, std::size_t alignment);

    // Reclaims an allocation only when it is the last allocation in its storage.
    void release_last(void *address, std::size_t size) noexcept;

    // Uses caller-owned storage first and falls back to retained scratch storage when it is full.
    LocalStorageScope use_local_storage(std::span<std::byte> storage) noexcept;

  private:
    friend EvaluationContext;
    friend RuntimeGraph;

    explicit ScratchPad(details::EvaluationScratch *evaluation_scratch) noexcept
        : evaluation_scratch_{evaluation_scratch} {}

    [[nodiscard]] void *allocate_from_local_storage(std::size_t size, std::size_t alignment) noexcept;
    bool release_last_from_local_storage(void *address, std::size_t size) noexcept;

    [[nodiscard]] void *allocate_from_storage(std::size_t size, std::size_t alignment);
    void release_last_from_storage(void *address, std::size_t size) noexcept;
    void ensure_frame();
    void reset() noexcept;

    details::EvaluationScratch *evaluation_scratch_;
    void *frame_{};
    LocalStorageScope *local_storage_{};
};

inline ScratchPad::LocalStorageScope::LocalStorageScope(ScratchPad &scratch_pad,
                                                        std::span<std::byte> storage) noexcept
    : scratch_pad_{&scratch_pad}, previous_{scratch_pad.local_storage_}, storage_{storage} {
    scratch_pad.local_storage_ = this;
}

inline ScratchPad::LocalStorageScope::~LocalStorageScope() {
    scratch_pad_->local_storage_ = previous_;
}

inline void *ScratchPad::allocate(std::size_t size, std::size_t alignment) {
    if (auto *result = allocate_from_local_storage(size, alignment); result != nullptr) {
        return result;
    }

    return allocate_from_storage(size, alignment);
}

inline void ScratchPad::release_last(void *address, std::size_t size) noexcept {
    if (release_last_from_local_storage(address, size)) {
        return;
    }

    release_last_from_storage(address, size);
}

inline ScratchPad::LocalStorageScope ScratchPad::use_local_storage(std::span<std::byte> storage) noexcept {
    return LocalStorageScope{*this, storage};
}

inline void *ScratchPad::allocate_from_local_storage(std::size_t size, std::size_t alignment) noexcept {
    if (local_storage_ == nullptr || local_storage_->used_ == local_storage_->storage_.size()) {
        return nullptr;
    }

    auto *begin = local_storage_->storage_.data();
    auto *result = static_cast<void *>(begin + local_storage_->used_);
    auto available = local_storage_->storage_.size() - local_storage_->used_;
    if (std::align(alignment, size, result, available) == nullptr) {
        // Keep later allocations in retained storage after the first fallback.
        local_storage_->used_ = local_storage_->storage_.size();
        return nullptr;
    }

    local_storage_->used_ = static_cast<std::size_t>(static_cast<std::byte *>(result) - begin) + size;
    return result;
}

inline bool ScratchPad::release_last_from_local_storage(void *address, std::size_t size) noexcept {
    if (local_storage_ == nullptr) {
        return false;
    }

    const auto begin = reinterpret_cast<std::uintptr_t>(local_storage_->storage_.data());
    const auto end = begin + local_storage_->storage_.size();
    const auto allocation = reinterpret_cast<std::uintptr_t>(address);
    if (allocation < begin || allocation >= end) {
        return false;
    }

    if (allocation + size == begin + local_storage_->used_) {
        local_storage_->used_ = allocation - begin;
    }
    return true;
}

} // namespace bg

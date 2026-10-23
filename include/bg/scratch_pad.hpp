#pragma once

#include <cstddef>
#include <memory>

namespace bg {

namespace details {
class EvaluationScratch;
}

class EvaluationContext;
class RuntimeGraph;

// Provides temporary storage for one node invocation.
class ScratchPad {
  public:
    ~ScratchPad();

    ScratchPad(const ScratchPad &) = delete;
    ScratchPad &operator=(const ScratchPad &) = delete;
    ScratchPad(ScratchPad &&other) noexcept;
    ScratchPad &operator=(ScratchPad &&) = delete;

    [[nodiscard]] void *allocate(std::size_t size, std::size_t alignment);

    // Reclaims an allocation only when it is the last allocation in its storage.
    void release_last(void *address, std::size_t size) noexcept;

  private:
    friend EvaluationContext;
    friend RuntimeGraph;

    explicit ScratchPad(details::EvaluationScratch *evaluation_scratch) noexcept
        : evaluation_scratch_{evaluation_scratch} {}

    void ensure_frame();
    void reset() noexcept;

    details::EvaluationScratch *evaluation_scratch_;
    void *frame_{};
};

} // namespace bg

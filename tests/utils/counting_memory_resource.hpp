#pragma once

#include <cstddef>
#include <memory_resource>

class counting_memory_resource final : public std::pmr::memory_resource {
  public:
    [[nodiscard]] std::size_t allocations() const noexcept { return allocations_; }
    [[nodiscard]] std::size_t allocated_bytes() const noexcept { return allocated_bytes_; }

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *result = upstream_->allocate(bytes, alignment);
        ++allocations_;
        allocated_bytes_ += bytes;

        return result;
    }

    void do_deallocate(void *memory, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(memory, bytes, alignment);
        --allocations_;
        allocated_bytes_ -= bytes;
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource *upstream_{std::pmr::new_delete_resource()};
    std::size_t allocations_{};
    std::size_t allocated_bytes_{};
};

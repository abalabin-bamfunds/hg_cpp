#pragma once

#include <bg/util/details/bunch_blocks.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <utility>

namespace bg::util {

// Stores positions [0, size) without relocating live values when storage grows.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename T, std::uint32_t InitialElements = 8> class contiguous_bunch {
  public:
    using size_type = std::uint32_t;

    contiguous_bunch() = default;

    contiguous_bunch(const contiguous_bunch &) = delete;
    contiguous_bunch &operator=(const contiguous_bunch &) = delete;
    contiguous_bunch(contiguous_bunch &&) = delete;
    contiguous_bunch &operator=(contiguous_bunch &&) = delete;

    ~contiguous_bunch() { assert(size_ == 0); }

    template <typename... Arguments>
    T &emplace_back(std::pmr::memory_resource &memory, Arguments &&...arguments) {
        auto *position = blocks_.ensure(size_, memory);

        try {
            std::construct_at(position, std::forward<Arguments>(arguments)...);
        } catch (...) {
            blocks_.release_unused(size_, memory);
            throw;
        }

        blocks_.added(size_);
        ++size_;

        return *position;
    }

    void pop_back(std::pmr::memory_resource &memory) noexcept {
        assert(size_ != 0);

        const auto position = --size_;
        std::destroy_at(blocks_.get(position));
        blocks_.removed(position, memory);
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        while (size_ != 0) {
            pop_back(memory);
        }

        blocks_.release(memory);
    }

    [[nodiscard]] T &operator[](size_type position) noexcept {
        assert(position < size_);
        return *blocks_.get(position);
    }

    [[nodiscard]] const T &operator[](size_type position) const noexcept {
        assert(position < size_);
        return *blocks_.get(position);
    }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type allocated_block_count() const noexcept { return blocks_.allocated_block_count(); }

  private:
    details::bunch_blocks<T, InitialElements> blocks_;
    size_type size_{};
};

} // namespace bg::util

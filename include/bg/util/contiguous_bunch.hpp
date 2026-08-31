#pragma once

#include <bg/util/details/bunch_blocks.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <cstdint>
#include <functional>
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

    template <typename... Arguments> T &emplace_back(std::pmr::memory_resource &memory, Arguments &&...arguments) {
        return emplace_back_with(
            memory, [&](T *position) { std::construct_at(position, std::forward<Arguments>(arguments)...); });
    }

    template <typename Constructor>
        requires std::invocable<Constructor &, T *>
    T &emplace_back_with(std::pmr::memory_resource &memory, Constructor &&construct) {
        auto *position = blocks_.ensure(size_, memory);
        auto release = scope_exit{[&]() noexcept { blocks_.release_unused(size_, memory); }};
        std::invoke(construct, position);
        release.release();

        blocks_.added(size_);
        ++size_;

        return *position;
    }

    void pop_back(std::pmr::memory_resource &memory) noexcept {
        pop_back_with(memory, [](T *value) noexcept { std::destroy_at(value); });
    }

    template <typename Destructor>
        requires std::invocable<Destructor &, T *>
    void pop_back_with(std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        assert(size_ != 0);

        const auto position = --size_;
        std::invoke(destroy, blocks_.get(position));
        blocks_.removed(position, memory);
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        release_with(memory, [](T *value) noexcept { std::destroy_at(value); });
    }

    template <typename Destructor>
        requires std::invocable<Destructor &, T *>
    void release_with(std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        while (size_ != 0) {
            pop_back_with(memory, destroy);
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

    void swap(contiguous_bunch &other) noexcept {
        blocks_.swap(other.blocks_);
        std::swap(size_, other.size_);
    }

  private:
    details::bunch_blocks<T, InitialElements> blocks_;
    size_type size_{};
};

} // namespace bg::util

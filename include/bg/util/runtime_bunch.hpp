#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <memory_resource>
#include <utility>

namespace bg::util {

struct runtime_item_layout {
    static constexpr auto maximum_block_bytes = std::size_t{256} * 1024;
    static constexpr auto initial_element_count = std::uint32_t{8};

    constexpr runtime_item_layout(std::size_t item_size, std::size_t item_alignment) noexcept
        : size{item_size}, alignment{item_alignment},
          maximum_elements{static_cast<std::uint32_t>(std::bit_floor(std::max(maximum_block_bytes / size, 1UZ)))},
          growth_levels{static_cast<std::uint32_t>(
              std::countr_zero(maximum_elements / std::min(initial_element_count, maximum_elements)))},
          first_capped_position{static_cast<std::uint64_t>(std::min(initial_element_count, maximum_elements)) *
                                ((std::uint64_t{1} << growth_levels) - 1)} {}

    const std::size_t size;
    const std::size_t alignment;
    const std::uint32_t maximum_elements;
    const std::uint32_t growth_levels;
    const std::uint64_t first_capped_position;
};

namespace details {

// Stores fixed-layout items without making their C++ type part of the bunch type.
class runtime_bunch_blocks {
    using size_type = std::uint32_t;

    struct location {
        size_type block;
        size_type offset;
    };

    struct block {
        std::byte *data{};
        size_type constructed{};
    };

  public:
    runtime_bunch_blocks() = default;

    runtime_bunch_blocks(const runtime_bunch_blocks &) = delete;
    runtime_bunch_blocks &operator=(const runtime_bunch_blocks &) = delete;
    runtime_bunch_blocks(runtime_bunch_blocks &&) = delete;
    runtime_bunch_blocks &operator=(runtime_bunch_blocks &&) = delete;

    ~runtime_bunch_blocks() {
        assert(first_.data == nullptr);
        assert(additional_ == nullptr);
    }

    [[nodiscard]] void *get(size_type position, const runtime_item_layout &layout) noexcept {
        const auto [block_index, offset] = locate(position, layout);
        auto &current = block_at(block_index);

        return current.data == nullptr ? nullptr : current.data + offset * layout.size;
    }

    [[nodiscard]] const void *get(size_type position, const runtime_item_layout &layout) const noexcept {
        const auto [block_index, offset] = locate(position, layout);
        const auto &current = block_at(block_index);

        return current.data == nullptr ? nullptr : current.data + offset * layout.size;
    }

    [[nodiscard]] void *ensure(size_type position, const runtime_item_layout &layout,
                               std::pmr::memory_resource &memory) {
        const auto [block_index, offset] = locate(position, layout);
        auto &current = ensure_descriptor(block_index, memory);

        if (current.data == nullptr) {
            const auto bytes = layout.size * block_capacity(block_index, layout);
            current.data = static_cast<std::byte *>(memory.allocate(bytes, layout.alignment));
            ++allocated_blocks_;
        }

        return current.data + offset * layout.size;
    }

    void added(size_type position, const runtime_item_layout &layout) noexcept {
        ++block_at(locate(position, layout).block).constructed;
    }

    void removed(size_type position, const runtime_item_layout &layout, std::pmr::memory_resource &memory) noexcept {
        const auto block_index = locate(position, layout).block;
        auto &current = block_at(block_index);

        assert(current.constructed != 0);
        if (--current.constructed == 0) {
            release_block(block_index, layout, memory);
        }
    }

    void release_unused(size_type position, const runtime_item_layout &layout,
                        std::pmr::memory_resource &memory) noexcept {
        const auto block_index = locate(position, layout).block;
        auto &current = block_at(block_index);

        if (current.constructed == 0) {
            release_block(block_index, layout, memory);
        }
    }

    void release(const runtime_item_layout &layout, std::pmr::memory_resource &memory) noexcept {
        release_block(0, layout, memory);
        for (auto i = size_type{}; i < additional_size_; ++i) {
            release_block(i + 1, layout, memory);
        }

        if (additional_ != nullptr) {
            memory.deallocate(additional_, sizeof(block) * additional_capacity_, alignof(block));
        }

        additional_ = nullptr;
        additional_size_ = 0;
        additional_capacity_ = 0;
    }

    [[nodiscard]] size_type allocated_block_count() const noexcept { return allocated_blocks_; }

    void swap(runtime_bunch_blocks &other) noexcept {
        using std::swap;
        swap(first_, other.first_);
        swap(additional_, other.additional_);
        swap(additional_size_, other.additional_size_);
        swap(additional_capacity_, other.additional_capacity_);
        swap(allocated_blocks_, other.allocated_blocks_);
    }

  private:
    [[nodiscard]] static location locate(size_type position, const runtime_item_layout &layout) noexcept {
        const auto initial = std::min(runtime_item_layout::initial_element_count, layout.maximum_elements);
        if (position < layout.first_capped_position) {
            const auto scaled = position / initial + 1;
            const auto block_index = static_cast<size_type>(std::bit_width(scaled) - 1);
            const auto first = static_cast<std::uint64_t>(initial) * ((std::uint64_t{1} << block_index) - 1);

            return {block_index, static_cast<size_type>(position - first)};
        }

        const auto after_growth = static_cast<std::uint64_t>(position) - layout.first_capped_position;
        const auto block_index = layout.growth_levels + after_growth / layout.maximum_elements;

        return {static_cast<size_type>(block_index), static_cast<size_type>(after_growth % layout.maximum_elements)};
    }

    [[nodiscard]] static size_type block_capacity(size_type block_index, const runtime_item_layout &layout) noexcept {
        if (block_index < layout.growth_levels) {
            return std::min(runtime_item_layout::initial_element_count, layout.maximum_elements) << block_index;
        }

        return layout.maximum_elements;
    }

    [[nodiscard]] block &block_at(size_type block_index) noexcept {
        return block_index == 0 ? first_ : additional_[block_index - 1];
    }

    [[nodiscard]] const block &block_at(size_type block_index) const noexcept {
        return block_index == 0 ? first_ : additional_[block_index - 1];
    }

    [[nodiscard]] block &ensure_descriptor(size_type block_index, std::pmr::memory_resource &memory) {
        if (block_index == 0) {
            return first_;
        }

        if (block_index > additional_capacity_) {
            grow_descriptors(block_index, memory);
        }

        while (additional_size_ < block_index) {
            std::construct_at(additional_ + additional_size_);
            ++additional_size_;
        }

        return additional_[block_index - 1];
    }

    void grow_descriptors(size_type required, std::pmr::memory_resource &memory) {
        auto capacity = std::max<size_type>(additional_capacity_, 4);
        while (capacity < required) {
            if (capacity > std::numeric_limits<size_type>::max() / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }

        auto *replacement = static_cast<block *>(memory.allocate(sizeof(block) * capacity, alignof(block)));
        for (auto i = size_type{}; i < additional_size_; ++i) {
            std::construct_at(replacement + i, additional_[i]);
        }

        if (additional_ != nullptr) {
            memory.deallocate(additional_, sizeof(block) * additional_capacity_, alignof(block));
        }

        additional_ = replacement;
        additional_capacity_ = capacity;
    }

    void release_block(size_type block_index, const runtime_item_layout &layout,
                       std::pmr::memory_resource &memory) noexcept {
        auto &current = block_at(block_index);
        if (current.data == nullptr) {
            return;
        }

        assert(current.constructed == 0);
        memory.deallocate(current.data, layout.size * block_capacity(block_index, layout), layout.alignment);
        current.data = nullptr;
        --allocated_blocks_;
    }

    block first_;
    block *additional_{};
    size_type additional_size_{};
    size_type additional_capacity_{};
    size_type allocated_blocks_{};
};

} // namespace details

class runtime_contiguous_bunch {
  public:
    using size_type = std::uint32_t;

    template <typename Constructor>
    void *emplace_back_with(const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                            Constructor &&construct) {
        auto *item = blocks_.ensure(size_, layout, memory);
        try {
            std::invoke(construct, item);
        } catch (...) {
            blocks_.release_unused(size_, layout, memory);
            throw;
        }

        blocks_.added(size_, layout);
        ++size_;
        return item;
    }

    template <typename Destructor>
    void pop_back_with(const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                       Destructor &&destroy) noexcept {
        assert(size_ != 0);
        const auto position = --size_;
        std::invoke(destroy, blocks_.get(position, layout));
        blocks_.removed(position, layout, memory);
    }

    template <typename Destructor>
    void release_with(const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                      Destructor &&destroy) noexcept {
        while (size_ != 0) {
            pop_back_with(layout, memory, destroy);
        }
        blocks_.release(layout, memory);
    }

    [[nodiscard]] void *get(size_type position, const runtime_item_layout &layout) noexcept {
        assert(position < size_);
        return blocks_.get(position, layout);
    }

    [[nodiscard]] const void *get(size_type position, const runtime_item_layout &layout) const noexcept {
        assert(position < size_);
        return blocks_.get(position, layout);
    }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type allocated_block_count() const noexcept { return blocks_.allocated_block_count(); }

    void swap(runtime_contiguous_bunch &other) noexcept {
        blocks_.swap(other.blocks_);
        std::swap(size_, other.size_);
    }

  private:
    details::runtime_bunch_blocks blocks_;
    size_type size_{};
};

class runtime_sparse_bunch {
  public:
    using size_type = std::uint32_t;

    template <typename Constructor>
    void *emplace_at_with(size_type position, const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                          Constructor &&construct) {
        auto *item = blocks_.ensure(position, layout, memory);
        try {
            std::invoke(construct, item);
        } catch (...) {
            blocks_.release_unused(position, layout, memory);
            throw;
        }

        blocks_.added(position, layout);
        return item;
    }

    template <typename Destructor>
    void erase_at_with(size_type position, const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                       Destructor &&destroy) noexcept {
        auto *item = blocks_.get(position, layout);
        assert(item != nullptr);
        std::invoke(destroy, item);
        blocks_.removed(position, layout, memory);
    }

    template <typename Positions, typename Destructor>
    void release_with(const Positions &positions, const runtime_item_layout &layout, std::pmr::memory_resource &memory,
                      Destructor &&destroy) noexcept {
        for (const auto position : positions) {
            auto *item = blocks_.get(position, layout);
            std::invoke(destroy, item);
            blocks_.removed(position, layout, memory);
        }
        blocks_.release(layout, memory);
    }

    [[nodiscard]] void *get(size_type position, const runtime_item_layout &layout) noexcept {
        return blocks_.get(position, layout);
    }

    [[nodiscard]] const void *get(size_type position, const runtime_item_layout &layout) const noexcept {
        return blocks_.get(position, layout);
    }

    [[nodiscard]] size_type allocated_block_count() const noexcept { return blocks_.allocated_block_count(); }

    void swap(runtime_sparse_bunch &other) noexcept { blocks_.swap(other.blocks_); }

  private:
    details::runtime_bunch_blocks blocks_;
};

} // namespace bg::util

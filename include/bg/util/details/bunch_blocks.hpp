#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <utility>

namespace bg::util::details {

template <typename T, std::uint32_t InitialElements = 8> class bunch_blocks {
  public:
    using size_type = std::uint32_t;

    struct location {
        size_type block;
        size_type offset;
    };

  private:
    static constexpr auto maximum_block_bytes = std::size_t{256} * 1024;
    static constexpr auto maximum_elements = [] {
        const auto count = std::max<std::size_t>(maximum_block_bytes / sizeof(T), 1);

        return static_cast<size_type>(std::bit_floor(count));
    }();
    static constexpr auto initial_elements = std::min<size_type>(InitialElements, maximum_elements);
    static_assert(initial_elements != 0 && std::has_single_bit(initial_elements));
    static constexpr auto growth_levels = std::countr_zero(maximum_elements / initial_elements);
    static constexpr auto first_capped_position =
        static_cast<std::uint64_t>(initial_elements) * ((std::uint64_t{1} << growth_levels) - 1);

    struct block {
        T *data{};
        size_type constructed{};
    };

  public:
    bunch_blocks() = default;

    bunch_blocks(const bunch_blocks &) = delete;
    bunch_blocks &operator=(const bunch_blocks &) = delete;
    bunch_blocks(bunch_blocks &&) = delete;
    bunch_blocks &operator=(bunch_blocks &&) = delete;

    ~bunch_blocks() {
        assert(first_.data == nullptr);
        assert(additional_ == nullptr);
    }

    [[nodiscard]] static constexpr location locate(size_type position) noexcept {
        if (position < first_capped_position) {
            const auto scaled = position / initial_elements + 1;
            const auto block_index = static_cast<size_type>(std::bit_width(scaled) - 1);
            const auto first = static_cast<std::uint64_t>(initial_elements) * ((std::uint64_t{1} << block_index) - 1);

            return {block_index, static_cast<size_type>(position - first)};
        }

        const auto after_growth = static_cast<std::uint64_t>(position) - first_capped_position;
        const auto block_index = growth_levels + after_growth / maximum_elements;

        return {static_cast<size_type>(block_index), static_cast<size_type>(after_growth % maximum_elements)};
    }

    [[nodiscard]] static constexpr size_type block_capacity(size_type block_index) noexcept {
        if (block_index < growth_levels) {
            return initial_elements << block_index;
        }

        return maximum_elements;
    }

    [[nodiscard]] T *get(size_type position) noexcept {
        const auto [block_index, offset] = locate(position);
        auto &current = block_at(block_index);

        return current.data == nullptr ? nullptr : current.data + offset;
    }

    [[nodiscard]] const T *get(size_type position) const noexcept {
        const auto [block_index, offset] = locate(position);
        const auto &current = block_at(block_index);

        return current.data == nullptr ? nullptr : current.data + offset;
    }

    [[nodiscard]] T *ensure(size_type position, std::pmr::memory_resource &memory) {
        const auto [block_index, offset] = locate(position);
        auto &current = ensure_descriptor(block_index, memory);

        if (current.data == nullptr) {
            const auto capacity = block_capacity(block_index);
            current.data = static_cast<T *>(memory.allocate(sizeof(T) * capacity, alignof(T)));
            ++allocated_blocks_;
        }

        return current.data + offset;
    }

    void added(size_type position) noexcept { ++block_at(locate(position).block).constructed; }

    void removed(size_type position, std::pmr::memory_resource &memory) noexcept {
        const auto block_index = locate(position).block;
        auto &current = block_at(block_index);

        assert(current.constructed != 0);
        if (--current.constructed == 0) {
            release_block(block_index, memory);
        }
    }

    void release_unused(size_type position, std::pmr::memory_resource &memory) noexcept {
        const auto block_index = locate(position).block;
        auto &current = block_at(block_index);

        if (current.constructed == 0) {
            release_block(block_index, memory);
        }
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        release_block(0, memory);

        for (auto i = size_type{}; i < additional_size_; ++i) {
            release_block(i + 1, memory);
        }

        if (additional_ != nullptr) {
            memory.deallocate(additional_, sizeof(block) * additional_capacity_, alignof(block));
        }

        additional_ = nullptr;
        additional_size_ = 0;
        additional_capacity_ = 0;
    }

    [[nodiscard]] size_type allocated_block_count() const noexcept { return allocated_blocks_; }

    void swap(bunch_blocks &other) noexcept {
        using std::swap;
        swap(first_, other.first_);
        swap(additional_, other.additional_);
        swap(additional_size_, other.additional_size_);
        swap(additional_capacity_, other.additional_capacity_);
        swap(allocated_blocks_, other.allocated_blocks_);
    }

  private:
    [[nodiscard]] block &block_at(size_type block_index) noexcept {
        if (block_index == 0) {
            return first_;
        }

        assert(block_index <= additional_size_);
        return additional_[block_index - 1];
    }

    [[nodiscard]] const block &block_at(size_type block_index) const noexcept {
        if (block_index == 0) {
            return first_;
        }

        assert(block_index <= additional_size_);
        return additional_[block_index - 1];
    }

    [[nodiscard]] block &ensure_descriptor(size_type block_index, std::pmr::memory_resource &memory) {
        if (block_index == 0) {
            return first_;
        }

        const auto required = block_index;
        if (required > additional_capacity_) {
            grow_descriptors(required, memory);
        }

        while (additional_size_ < required) {
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

    void release_block(size_type block_index, std::pmr::memory_resource &memory) noexcept {
        auto &current = block_at(block_index);
        if (current.data == nullptr) {
            return;
        }

        assert(current.constructed == 0);
        memory.deallocate(current.data, sizeof(T) * block_capacity(block_index), alignof(T));
        current.data = nullptr;
        --allocated_blocks_;
    }

    block first_;
    block *additional_{};
    size_type additional_size_{};
    size_type additional_capacity_{};
    size_type allocated_blocks_{};
};

} // namespace bg::util::details

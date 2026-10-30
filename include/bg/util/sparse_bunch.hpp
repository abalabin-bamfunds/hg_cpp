#pragma once

#include <bg/util/details/bunch_blocks.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <memory_resource>
#include <ranges>
#include <type_traits>
#include <utility>

namespace bg::util {

// Stores one typed column whose constructed positions are described externally.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename T, std::uint32_t InitialElements = 8> class sparse_bunch {
  public:
    using size_type = std::uint32_t;
    using const_accessor = typename details::bunch_blocks<T, InitialElements>::const_accessor;
    using storage_block = ::bg::util::storage_block;

    sparse_bunch() = default;

    sparse_bunch(const sparse_bunch &) = delete;
    sparse_bunch &operator=(const sparse_bunch &) = delete;
    sparse_bunch(sparse_bunch &&) = delete;
    sparse_bunch &operator=(sparse_bunch &&) = delete;

    template <typename... Arguments>
    T &emplace_at(size_type position, std::pmr::memory_resource &memory, Arguments &&...arguments) {
        return emplace_at_with(position, memory,
                               [&](T *value) { std::construct_at(value, std::forward<Arguments>(arguments)...); });
    }

    template <typename Constructor>
        requires std::invocable<Constructor &, T *>
    T &emplace_at_with(size_type position, std::pmr::memory_resource &memory, Constructor &&construct) {
        auto *value = blocks_.ensure(position, memory);
        auto release = scope_exit{[&]() noexcept { blocks_.release_unused(position, memory); }};
        std::invoke(construct, value);
        release.release();

        blocks_.added(position);

        return *value;
    }

    void erase_at(size_type position, std::pmr::memory_resource &memory) noexcept {
        erase_at_with(position, memory, [](T *value) noexcept { std::destroy_at(value); });
    }

    template <typename Destructor>
        requires std::invocable<Destructor &, T *>
    void erase_at_with(size_type position, std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        auto *value = blocks_.get(position);
        assert(value != nullptr);

        std::invoke(destroy, value);
        blocks_.removed(position, memory);
    }

    // Moves constructed cells between ranges in one allocated block without allocation. Source cells must be
    // constructed and destination-only cells must be unconstructed. Overlapping ranges are supported. The
    // block's constructed count is unchanged.
    void relocate(size_type source, size_type destination, size_type count) noexcept
        requires std::is_trivially_copyable_v<T>
    {
        if (count == 0 || source == destination) {
            return;
        }

        const auto source_end = std::uint64_t{source} + count;
        const auto destination_end = std::uint64_t{destination} + count;
#ifndef NDEBUG
        const auto source_block = blocks_.block_containing(source);
        assert(source >= source_block.first);
        assert(source_end <= source_block.end);
        assert(destination >= source_block.first);
        assert(destination_end <= source_block.end);
        assert(source_block.data != nullptr);
#endif

        auto *source_cells = blocks_.get(source);
        auto *destination_cells = blocks_.get(destination);
        assert(source_cells != nullptr);
        assert(destination_cells != nullptr);
        std::memmove(destination_cells, source_cells, sizeof(T) * count);

        auto clear_start = std::uint64_t{};
        auto clear_end = std::uint64_t{};
        if (destination < source) {
            clear_start = std::max(std::uint64_t{source}, destination_end);
            clear_end = source_end;
        } else {
            clear_start = source;
            clear_end = std::min(source_end, std::uint64_t{destination});
        }

        if (clear_start != clear_end) {
            std::destroy_n(blocks_.get(static_cast<size_type>(clear_start)),
                           static_cast<size_type>(clear_end - clear_start));
        }
    }

    // The range must enumerate each constructed position exactly once, in any order.
    // Iteration and destruction must not throw.
    template <typename Positions>
        requires std::ranges::input_range<const Positions> &&
                 std::convertible_to<std::ranges::range_reference_t<const Positions>, size_type>
    void release(const Positions &positions, std::pmr::memory_resource &memory) noexcept {
        release_with(positions, memory, [](T *value) noexcept { std::destroy_at(value); });
    }

    // Releases storage constructed at every position in [0, extent).
    void release_prefix(size_type extent, std::pmr::memory_resource &memory) noexcept {
        for (auto position = size_type{}; position != extent; ++position) {
            std::destroy_at(blocks_.get(position));
            blocks_.removed(position, memory);
        }

        blocks_.release(memory);
    }

    // Uses the same position range contract with a caller-provided destructor.
    template <typename Positions, typename Destructor>
        requires std::ranges::input_range<const Positions> &&
                 std::convertible_to<std::ranges::range_reference_t<const Positions>, size_type> &&
                 std::invocable<Destructor &, T *>
    void release_with(const Positions &positions, std::pmr::memory_resource &memory, Destructor &&destroy) noexcept {
        for (const auto position : positions) {
            std::invoke(destroy, blocks_.get(position));
            blocks_.removed(position, memory);
        }

        blocks_.release(memory);
    }

    T *get(size_type position, const occupancy_index &positions) noexcept {
        return positions.contains(position) ? blocks_.get(position) : nullptr;
    }

    const T *get(size_type position, const occupancy_index &positions) const noexcept {
        return positions.contains(position) ? blocks_.get(position) : nullptr;
    }

    T &operator[](size_type position) noexcept { return *blocks_.get(position); }
    const T &operator[](size_type position) const noexcept { return *blocks_.get(position); }

    const_accessor accessor() const noexcept { return blocks_.accessor(); }

    storage_block block_containing(size_type position) const noexcept { return blocks_.block_containing(position); }

    size_type allocated_block_count() const noexcept { return blocks_.allocated_block_count(); }

    void swap(sparse_bunch &other) noexcept { blocks_.swap(other.blocks_); }

  private:
    details::bunch_blocks<T, InitialElements> blocks_;
};

} // namespace bg::util

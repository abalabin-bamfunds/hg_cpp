#pragma once

#include <bg/util/details/bunch_blocks.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/scope_exit.hpp>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
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

    void release(const occupancy_index &positions, std::pmr::memory_resource &memory) noexcept {
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

    template <typename Destructor>
        requires std::invocable<Destructor &, T *>
    void release_with(const occupancy_index &positions, std::pmr::memory_resource &memory,
                      Destructor &&destroy) noexcept {
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

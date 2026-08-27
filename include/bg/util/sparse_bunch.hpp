#pragma once

#include <bg/util/details/bunch_blocks.hpp>
#include <bg/util/occupancy_index.hpp>

#include <cassert>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <utility>

namespace bg::util {

// Stores one typed column whose constructed positions are owned by an external occupancy index.
// The caller must use one memory resource for all storage changes and release storage before destruction.
template <typename T, std::uint32_t InitialElements = 8> class sparse_bunch {
  public:
    using size_type = std::uint32_t;

    sparse_bunch() = default;

    sparse_bunch(const sparse_bunch &) = delete;
    sparse_bunch &operator=(const sparse_bunch &) = delete;
    sparse_bunch(sparse_bunch &&) = delete;
    sparse_bunch &operator=(sparse_bunch &&) = delete;

    template <typename... Arguments>
    T &emplace_at(size_type position, std::pmr::memory_resource &memory, Arguments &&...arguments) {
        auto *value = blocks_.ensure(position, memory);

        try {
            std::construct_at(value, std::forward<Arguments>(arguments)...);
        } catch (...) {
            blocks_.release_unused(position, memory);
            throw;
        }

        blocks_.added(position);

        return *value;
    }

    void erase_at(size_type position, std::pmr::memory_resource &memory) noexcept {
        auto *value = blocks_.get(position);
        assert(value != nullptr);

        std::destroy_at(value);
        blocks_.removed(position, memory);
    }

    void release(const occupancy_index &positions, std::pmr::memory_resource &memory) noexcept {
        for (const auto position : positions) {
            std::destroy_at(blocks_.get(position));
            blocks_.removed(position, memory);
        }

        blocks_.release(memory);
    }

    [[nodiscard]] T *get(size_type position, const occupancy_index &positions) noexcept {
        return positions.contains(position) ? blocks_.get(position) : nullptr;
    }

    [[nodiscard]] const T *get(size_type position, const occupancy_index &positions) const noexcept {
        return positions.contains(position) ? blocks_.get(position) : nullptr;
    }

    [[nodiscard]] T &operator[](size_type position) noexcept { return *blocks_.get(position); }
    [[nodiscard]] const T &operator[](size_type position) const noexcept { return *blocks_.get(position); }

    [[nodiscard]] size_type allocated_block_count() const noexcept { return blocks_.allocated_block_count(); }

  private:
    details::bunch_blocks<T, InitialElements> blocks_;
};

} // namespace bg::util

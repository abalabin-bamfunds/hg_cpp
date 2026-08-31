#pragma once

#include <bg/util/index.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <type_traits>

namespace bg::util {

// Tracks occupied 32-bit positions and reuses the lowest available position.
// The caller must use one memory resource for all storage changes and release storage before destruction.
class occupancy_index {
  public:
    using size_type = std::uint32_t;
    using iterator = index<>::iterator;

    static constexpr auto npos = std::numeric_limits<size_type>::max();

    occupancy_index() = default;

    occupancy_index(const occupancy_index &) = delete;
    occupancy_index &operator=(const occupancy_index &) = delete;
    occupancy_index(occupancy_index &&) = delete;
    occupancy_index &operator=(occupancy_index &&) = delete;

    [[nodiscard]] size_type acquire(std::pmr::memory_resource &memory) {
        const auto position = next_free_;
        if (position == npos) {
            throw std::length_error{"occupancy index is full"};
        }

        positions_.set(position, memory);
        ++count_;
        next_free_ = positions_.first_unset(position + 1);

        return position;
    }

    bool set(size_type position, std::pmr::memory_resource &memory) {
        if (!positions_.set(position, memory)) {
            return false;
        }

        ++count_;
        if (position == next_free_) {
            next_free_ = positions_.first_unset(position + 1);
        }

        return true;
    }

    bool reset(size_type position) noexcept {
        if (!positions_.reset(position)) {
            return false;
        }

        --count_;
        next_free_ = std::min(next_free_, position);

        return true;
    }

    void trim(std::pmr::memory_resource &memory) noexcept {
        positions_.trim(memory);
        next_free_ = std::min(next_free_, positions_.size());
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        count_ = 0;
        next_free_ = 0;
        positions_.release(memory);
    }

    [[nodiscard]] bool contains(size_type position) const noexcept { return positions_.contains(position); }
    [[nodiscard]] size_type size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] size_type capacity() const noexcept { return positions_.capacity(); }

    void swap(occupancy_index &other) noexcept {
        positions_.swap(other.positions_);
        std::swap(count_, other.count_);
        std::swap(next_free_, other.next_free_);
    }

    [[nodiscard]] iterator begin() const noexcept { return positions_.begin(); }
    [[nodiscard]] iterator end() const noexcept { return positions_.end(); }

  private:
    index<> positions_;
    size_type count_{};
    size_type next_free_{};
};

static_assert(std::is_trivially_copyable_v<occupancy_index>);

} // namespace bg::util

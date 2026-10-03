#pragma once

#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
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
    static constexpr bool supports_byte_copy_initialization = true;

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

    bool contains(size_type position) const noexcept { return positions_.contains(position); }
    size_type size() const noexcept { return count_; }
    bool empty() const noexcept { return count_ == 0; }
    bool dense() const noexcept { return count_ == positions_.size(); }
    size_type extent() const noexcept { return positions_.size(); }
    size_type capacity() const noexcept { return positions_.capacity(); }

    // Calls function with each [first, end) live position run. The callback must not mutate this index.
    template <typename Function> void for_each_run(Function &&function) const {
        auto first = positions_.first_set();
        while (first != positions_.size()) {
            const auto end = positions_.first_unset(first);
            std::invoke(function, first, end);
            first = positions_.first_set(end);
        }
    }

    void swap(occupancy_index &other) noexcept {
        positions_.swap(other.positions_);
        std::swap(count_, other.count_);
        std::swap(next_free_, other.next_free_);
    }

    iterator begin() const noexcept { return positions_.begin(); }
    iterator end() const noexcept { return positions_.end(); }

  private:
    index<> positions_;
    size_type count_{};
    size_type next_free_{};
};

static_assert(byte_copy_initializable<occupancy_index>);

} // namespace bg::util

#pragma once

#include <bg/util/contiguous_bunch.hpp>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory_resource>
#include <stdexcept>

namespace bg::util {

// Tracks occupied 32-bit positions and reuses the lowest available position.
// The caller must use one memory resource for all storage changes and release storage before destruction.
class occupancy_index {
  public:
    using size_type = std::uint32_t;
    static constexpr auto npos = std::numeric_limits<size_type>::max();

    occupancy_index() = default;

    occupancy_index(const occupancy_index &) = delete;
    occupancy_index &operator=(const occupancy_index &) = delete;
    occupancy_index(occupancy_index &&) = delete;
    occupancy_index &operator=(occupancy_index &&) = delete;

    ~occupancy_index() { assert(count_ == 0); }

    [[nodiscard]] size_type acquire(std::pmr::memory_resource &memory) {
        for (auto word_index = next_free_word_; word_index < words_.size(); ++word_index) {
            const auto available = ~words_[word_index];
            if (available == 0) {
                continue;
            }

            const auto bit = static_cast<size_type>(std::countr_zero(available));
            const auto position = word_index * bits_per_word + bit;
            set_existing(position);

            return position;
        }

        if (words_.size() == maximum_words) {
            throw std::length_error{"occupancy index is full"};
        }

        const auto word_index = words_.size();
        words_.emplace_back(memory, std::uint64_t{1});
        ++count_;
        next_free_word_ = word_index;

        return word_index * bits_per_word;
    }

    [[nodiscard]] bool set(size_type position, std::pmr::memory_resource &memory) {
        if (position == npos) {
            throw std::length_error{"invalid occupancy index position"};
        }

        const auto word_index = position / bits_per_word;
        while (words_.size() <= word_index) {
            words_.emplace_back(memory, std::uint64_t{});
        }

        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        if ((words_[word_index] & mask) != 0) {
            return false;
        }

        words_[word_index] |= mask;
        ++count_;
        update_next_free_word(word_index);

        return true;
    }

    [[nodiscard]] bool reset(size_type position) noexcept {
        const auto word_index = position / bits_per_word;
        if (word_index >= words_.size()) {
            return false;
        }

        const auto mask = std::uint64_t{1} << (position % bits_per_word);
        if ((words_[word_index] & mask) == 0) {
            return false;
        }

        words_[word_index] &= ~mask;
        --count_;
        next_free_word_ = std::min(next_free_word_, word_index);

        return true;
    }

    void trim(std::pmr::memory_resource &memory) noexcept {
        while (!words_.empty() && words_[words_.size() - 1] == 0) {
            words_.pop_back(memory);
        }

        next_free_word_ = std::min(next_free_word_, words_.size());
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        count_ = 0;
        next_free_word_ = 0;
        words_.release(memory);
    }

    [[nodiscard]] bool contains(size_type position) const noexcept {
        const auto word_index = position / bits_per_word;
        if (word_index >= words_.size()) {
            return false;
        }

        return (words_[word_index] & (std::uint64_t{1} << (position % bits_per_word))) != 0;
    }

    [[nodiscard]] size_type size() const noexcept { return count_; }
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] size_type capacity() const noexcept { return words_.size() * bits_per_word; }

    class iterator {
      public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = size_type;
        using difference_type = std::ptrdiff_t;

        iterator() = default;

        [[nodiscard]] value_type operator*() const noexcept {
            return word_index_ * bits_per_word + static_cast<size_type>(std::countr_zero(remaining_));
        }

        iterator &operator++() noexcept {
            remaining_ &= remaining_ - 1;
            advance();

            return *this;
        }

        iterator operator++(int) noexcept {
            auto previous = *this;
            ++*this;

            return previous;
        }

        bool operator==(const iterator &) const noexcept = default;

      private:
        friend class occupancy_index;

        iterator(const occupancy_index *index, size_type word_index) noexcept
            : index_{index}, word_index_{word_index} {
            if (word_index_ < index_->words_.size()) {
                remaining_ = index_->words_[word_index_];
            }

            advance();
        }

        void advance() noexcept {
            while (remaining_ == 0 && word_index_ < index_->words_.size()) {
                ++word_index_;
                if (word_index_ < index_->words_.size()) {
                    remaining_ = index_->words_[word_index_];
                }
            }
        }

        const occupancy_index *index_{};
        size_type word_index_{};
        std::uint64_t remaining_{};
    };

    [[nodiscard]] iterator begin() const noexcept { return iterator{this, 0}; }
    [[nodiscard]] iterator end() const noexcept { return iterator{this, words_.size()}; }

  private:
    static constexpr auto bits_per_word = size_type{64};
    static constexpr auto maximum_words = npos / bits_per_word + 1;

    void set_existing(size_type position) noexcept {
        const auto word_index = position / bits_per_word;
        words_[word_index] |= std::uint64_t{1} << (position % bits_per_word);
        ++count_;
        update_next_free_word(word_index);
    }

    void update_next_free_word(size_type word_index) noexcept {
        if (word_index != next_free_word_ || words_[word_index] != std::numeric_limits<std::uint64_t>::max()) {
            return;
        }

        while (next_free_word_ < words_.size() &&
               words_[next_free_word_] == std::numeric_limits<std::uint64_t>::max()) {
            ++next_free_word_;
        }
    }

    contiguous_bunch<std::uint64_t> words_;
    size_type count_{};
    size_type next_free_word_{};
};

} // namespace bg::util

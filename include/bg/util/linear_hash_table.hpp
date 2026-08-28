#pragma once

#include <bg/util/contiguous_bunch.hpp>
#include <bg/util/occupancy_index.hpp>
#include <bg/util/sparse_bunch.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <utility>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif

namespace bg::util {

// Splits or merges one bucket at a time, so capacity changes never process the complete table.
// The caller must use one memory resource for all storage changes and release storage before destruction.
class linear_hash_table {
  public:
    using hash_type = std::uint32_t;
    using position_type = std::uint32_t;
    using size_type = std::uint32_t;

  private:
    static constexpr auto entries_per_bucket = size_type{7};
    static constexpr auto npos = std::numeric_limits<size_type>::max();

    struct alignas(64) bucket {
        std::array<hash_type, entries_per_bucket> hashes{};
        std::array<position_type, entries_per_bucket> positions{};
        size_type next{npos};
        size_type size{};

        void append(hash_type hash, position_type position) noexcept {
            assert(size < entries_per_bucket);
            hashes[size] = hash;
            positions[size] = position;
            ++size;
        }

        void erase(size_type offset) noexcept {
            assert(offset < size);
            --size;
            hashes[offset] = hashes[size];
            positions[offset] = positions[size];
        }

        [[nodiscard]] size_type matching(hash_type hash) const noexcept {
            // The second vector includes one word after the hash array. The size mask removes that lane.
#if defined(__SSE2__) || defined(_M_X64)
            const auto expected = _mm_set1_epi32(std::bit_cast<std::int32_t>(hash));
            const auto first =
                _mm_cmpeq_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(hashes.data())), expected);
            auto second_values = __m128i{};
            std::memcpy(&second_values, reinterpret_cast<const std::byte *>(this) + 4 * sizeof(hash_type),
                        sizeof(second_values));
            const auto second = _mm_cmpeq_epi32(second_values, expected);
            const auto matches = static_cast<size_type>(_mm_movemask_ps(_mm_castsi128_ps(first))) |
                                 static_cast<size_type>(_mm_movemask_ps(_mm_castsi128_ps(second)) << 4);
#elif defined(__aarch64__) || defined(_M_ARM64)
            constexpr auto bit_values = std::array<std::uint32_t, 4>{1, 2, 4, 8};
            const auto expected = vdupq_n_u32(hash);
            const auto first = vceqq_u32(vld1q_u32(hashes.data()), expected);
            auto second_values = uint32x4_t{};
            std::memcpy(&second_values, reinterpret_cast<const std::byte *>(this) + 4 * sizeof(hash_type),
                        sizeof(second_values));
            const auto second = vceqq_u32(second_values, expected);
            const auto bits = vld1q_u32(bit_values.data());
            const auto matches = static_cast<size_type>(vaddvq_u32(vandq_u32(first, bits))) |
                                 static_cast<size_type>(vaddvq_u32(vandq_u32(second, bits)) << 4);
#else
            auto matches = size_type{};
            for (auto offset = size_type{}; offset < entries_per_bucket; ++offset) {
                matches |= static_cast<size_type>(hashes[offset] == hash) << offset;
            }
#endif

            return matches & ((size_type{1} << size) - 1);
        }
    };

    static_assert(sizeof(bucket) == 64);

  public:
    linear_hash_table() = default;

    linear_hash_table(const linear_hash_table &) = delete;
    linear_hash_table &operator=(const linear_hash_table &) = delete;
    linear_hash_table(linear_hash_table &&) = delete;
    linear_hash_table &operator=(linear_hash_table &&) = delete;

    template <std::predicate<position_type> Equal>
    [[nodiscard]] std::optional<position_type> find(hash_type hash, Equal equal) const {
        if (primary_.empty()) {
            return std::nullopt;
        }

        const auto *current = &primary_[bucket_for(hash)];
        while (true) {
            auto matching = current->matching(hash);
            while (matching != 0) {
                const auto offset = static_cast<size_type>(std::countr_zero(matching));
                if (equal(current->positions[offset])) {
                    return current->positions[offset];
                }

                matching &= matching - 1;
            }

            if (current->next == npos) {
                return std::nullopt;
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }
    }

    void insert(hash_type hash, position_type position, std::pmr::memory_resource &memory) {
        ensure_primary_bucket(memory);

        if ((static_cast<std::uint64_t>(size_) + 1) * maximum_load_denominator >
            static_cast<std::uint64_t>(bucket_count()) * entries_per_bucket * maximum_load_numerator) {
            split_one(memory);
        }

        append(primary_[bucket_for(hash)], hash, position, memory);
        ++size_;
    }

    [[nodiscard]] bool erase(hash_type hash, position_type position, std::pmr::memory_resource &memory) noexcept {
        if (primary_.empty()) {
            return false;
        }

        auto &head = primary_[bucket_for(hash)];
        auto *current = &head;
        auto *previous = static_cast<bucket *>(nullptr);
        auto current_position = npos;

        while (true) {
            for (auto i = size_type{}; i < current->size; ++i) {
                if (current->hashes[i] != hash || current->positions[i] != position) {
                    continue;
                }

                current->erase(i);
                --size_;

                if (current_position != npos && current->size == 0) {
                    previous->next = current->next;
                    overflow_.erase_at(current_position, memory);
                    const auto removed = overflow_positions_.reset(current_position);
                    assert(removed);
                    overflow_positions_.trim(memory);
                }

                return true;
            }

            if (current->next == npos) {
                return false;
            }

            previous = current;
            current_position = current->next;
            current = overflow_.get(current_position, overflow_positions_);
            assert(current != nullptr);
        }
    }

    // Merges only the final primary bucket. Returns false when the table cannot become smaller at its target load.
    [[nodiscard]] bool reduce(std::pmr::memory_resource &memory) {
        if (bucket_count() <= 1) {
            return false;
        }

        const auto target_bucket_count = bucket_count() - 1;
        if (static_cast<std::uint64_t>(size_) * maximum_load_denominator >
            static_cast<std::uint64_t>(target_bucket_count) * entries_per_bucket * maximum_load_numerator) {
            return false;
        }

        auto next_base = base_;
        auto next_split = split_;
        if (next_split == 0) {
            next_base /= 2;
            next_split = next_base;
        }
        --next_split;

        const auto source_index = next_base + next_split;
        auto &target = primary_[next_split];
        auto &source = primary_[source_index];
        ensure_capacity(target, chain_size(target) + chain_size(source), memory);

        move_chain(source, target);
        release_chain(source.next, memory);
        source.next = npos;
        primary_.pop_back(memory);

        base_ = next_base;
        split_ = next_split;

        return true;
    }

    void release(std::pmr::memory_resource &memory) noexcept {
        overflow_.release(overflow_positions_, memory);
        overflow_positions_.release(memory);
        primary_.release(memory);

        base_ = 1;
        split_ = 0;
        size_ = 0;
    }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] size_type bucket_count() const noexcept { return primary_.size(); }
    [[nodiscard]] size_type overflow_bucket_count() const noexcept { return overflow_positions_.size(); }

    void swap(linear_hash_table &other) noexcept {
        primary_.swap(other.primary_);
        overflow_positions_.swap(other.overflow_positions_);
        overflow_.swap(other.overflow_);
        std::swap(base_, other.base_);
        std::swap(split_, other.split_);
        std::swap(size_, other.size_);
    }

    [[nodiscard]] static constexpr hash_type hash_code(std::size_t hash) noexcept {
        if constexpr (sizeof(std::size_t) > sizeof(hash_type)) {
            hash ^= hash >> 32;
        }

        return static_cast<hash_type>(hash);
    }

  private:
    static constexpr auto maximum_load_numerator = std::uint64_t{3};
    static constexpr auto maximum_load_denominator = std::uint64_t{4};

    [[nodiscard]] size_type bucket_for(hash_type hash) const noexcept {
        auto result = hash & (base_ - 1);
        if (result < split_) {
            result = hash & ((base_ << 1) - 1);
        }

        return result;
    }

    void ensure_primary_bucket(std::pmr::memory_resource &memory) {
        if (primary_.empty()) {
            primary_.emplace_back(memory);
        }
    }

    void split_one(std::pmr::memory_resource &memory) {
        const auto source_index = split_;
        const auto new_index = base_ + split_;
        assert(new_index == primary_.size());

        auto &source = primary_[source_index];
        const auto moving = count_split_entries(source);
        auto &destination = primary_.emplace_back(memory);

        try {
            ensure_capacity(destination, moving, memory);
        } catch (...) {
            release_chain(destination.next, memory);
            destination.next = npos;
            primary_.pop_back(memory);
            throw;
        }

        move_split_entries(source, destination);
        remove_empty_overflow(source, memory);

        ++split_;
        if (split_ == base_) {
            base_ *= 2;
            split_ = 0;
        }
    }

    [[nodiscard]] size_type count_split_entries(const bucket &head) const noexcept {
        auto result = size_type{};
        auto *current = &head;

        while (true) {
            for (auto i = size_type{}; i < current->size; ++i) {
                result += (current->hashes[i] & base_) != 0;
            }

            if (current->next == npos) {
                return result;
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }
    }

    void move_split_entries(bucket &source, bucket &destination) noexcept {
        auto *current = &source;

        while (true) {
            for (auto i = size_type{}; i < current->size;) {
                if ((current->hashes[i] & base_) == 0) {
                    ++i;
                    continue;
                }

                append_existing(destination, current->hashes[i], current->positions[i]);
                current->erase(i);
            }

            if (current->next == npos) {
                return;
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }
    }

    void move_chain(bucket &source, bucket &destination) noexcept {
        auto *current = &source;

        while (true) {
            for (auto i = size_type{}; i < current->size; ++i) {
                append_existing(destination, current->hashes[i], current->positions[i]);
            }

            if (current->next == npos) {
                source.size = 0;
                return;
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }
    }

    void append(bucket &head, hash_type hash, position_type position, std::pmr::memory_resource &memory) {
        auto *current = &head;
        while (current->size == entries_per_bucket) {
            if (current->next == npos) {
                current->next = allocate_overflow(memory);
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }

        current->append(hash, position);
    }

    void append_existing(bucket &head, hash_type hash, position_type position) noexcept {
        auto *current = &head;
        while (current->size == entries_per_bucket) {
            assert(current->next != npos);
            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }

        current->append(hash, position);
    }

    [[nodiscard]] size_type allocate_overflow(std::pmr::memory_resource &memory) {
        const auto position = overflow_positions_.acquire(memory);

        try {
            overflow_.emplace_at(position, memory);
        } catch (...) {
            const auto removed = overflow_positions_.reset(position);
            assert(removed);
            overflow_positions_.trim(memory);
            throw;
        }

        return position;
    }

    void ensure_capacity(bucket &head, size_type entries, std::pmr::memory_resource &memory) {
        auto pages = size_type{1};
        auto *tail = &head;
        while (tail->next != npos) {
            tail = overflow_.get(tail->next, overflow_positions_);
            assert(tail != nullptr);
            ++pages;
        }

        const auto required = (entries + entries_per_bucket - 1) / entries_per_bucket;
        while (pages < required) {
            tail->next = allocate_overflow(memory);
            tail = overflow_.get(tail->next, overflow_positions_);
            ++pages;
        }
    }

    [[nodiscard]] size_type chain_size(const bucket &head) const noexcept {
        auto result = size_type{};
        auto *current = &head;

        while (true) {
            result += current->size;
            if (current->next == npos) {
                return result;
            }

            current = overflow_.get(current->next, overflow_positions_);
            assert(current != nullptr);
        }
    }

    void remove_empty_overflow(bucket &head, std::pmr::memory_resource &memory) noexcept {
        auto *current = &head;
        while (current->next != npos) {
            const auto next_position = current->next;
            auto *next = overflow_.get(next_position, overflow_positions_);
            assert(next != nullptr);

            if (next->size != 0) {
                current = next;
                continue;
            }

            current->next = next->next;
            overflow_.erase_at(next_position, memory);
            const auto removed = overflow_positions_.reset(next_position);
            assert(removed);
        }

        overflow_positions_.trim(memory);
    }

    void release_chain(size_type first, std::pmr::memory_resource &memory) noexcept {
        auto position = first;
        while (position != npos) {
            const auto *current = overflow_.get(position, overflow_positions_);
            assert(current != nullptr);
            const auto next = current->next;

            overflow_.erase_at(position, memory);
            const auto removed = overflow_positions_.reset(position);
            assert(removed);
            position = next;
        }

        overflow_positions_.trim(memory);
    }

    contiguous_bunch<bucket, 1> primary_;
    occupancy_index overflow_positions_;
    sparse_bunch<bucket, 1> overflow_;
    size_type base_{1};
    size_type split_{};
    size_type size_{};
};

} // namespace bg::util

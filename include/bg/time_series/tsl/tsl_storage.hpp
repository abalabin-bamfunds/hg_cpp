#pragma once

#include <bg/allocator.hpp>
#include <bg/evaluation_time.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

#include <cstddef>
#include <cstdint>

namespace bg::details {

template <std::size_t Size> class fixed_time_series_list_changes {
  public:
    bool mark(std::size_t index, EvaluationTime evaluation_time) noexcept {
        if (time_ != evaluation_time) {
            updated_.clear();
            time_ = evaluation_time;
        }

        return updated_.set(static_cast<std::uint32_t>(index));
    }

    bool contains(std::size_t index) const noexcept {
        return updated_.contains(static_cast<std::uint32_t>(index));
    }

    bool contains(std::size_t index, EvaluationTime evaluation_time) const noexcept {
        return time_ == evaluation_time && contains(index);
    }

    auto begin() const noexcept { return updated_.begin(); }
    auto end() const noexcept { return updated_.end(); }

    EvaluationTime time() const noexcept { return time_; }

  private:
    static_assert(Size < util::dynamic_index_extent);

    EvaluationTime time_{MIN_DT};
    util::index<static_cast<std::uint32_t>(Size)> updated_;
};

struct time_series_list_storage {
    static constexpr bool supports_byte_copy_initialization = true;

    EvaluationTime change_time{MIN_DT};
    util::index<> updated;

    [[nodiscard]] bool mark(std::size_t index, EvaluationTime evaluation_time, allocator &memory) noexcept {
        if (change_time != evaluation_time) {
            updated.clear();
            change_time = evaluation_time;
        }

        return updated.set(static_cast<std::uint32_t>(index), memory);
    }
};

static_assert(util::byte_copy_initializable<time_series_list_storage>);

} // namespace bg::details

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <vector>

namespace bg::details {

// Provides stable temporary storage for nested node invocations. Frames release their allocations together.
class EvaluationScratch {
    static constexpr auto initial_block_size = std::size_t{64} * 1024;
    static constexpr auto no_block = std::numeric_limits<std::size_t>::max();

    struct Position {
        std::size_t block;
        std::size_t used;
    };

  public:
    struct Frame {
        EvaluationScratch *scratch;
        void *first{};
        void *last{};

      private:
        friend EvaluationScratch;

        Frame(EvaluationScratch &scratch, Position position) noexcept : scratch{&scratch}, position{position} {}

        Position position;
    };

    explicit EvaluationScratch(std::pmr::memory_resource &memory) noexcept : memory_{&memory}, blocks_{&memory} {}

    ~EvaluationScratch() {
        for (const auto [data, size] : blocks_) {
            memory_->deallocate(data, size, alignof(std::max_align_t));
        }
    }

    EvaluationScratch(const EvaluationScratch &) = delete;
    EvaluationScratch &operator=(const EvaluationScratch &) = delete;

    [[nodiscard]] Frame &push_frame() {
        const auto position = current_position();
        auto *frame = ::new (allocate(sizeof(Frame), alignof(Frame))) Frame{*this, position};
        return *frame;
    }

    void pop_frame(Frame &frame) noexcept {
        const auto position = frame.position;
        std::destroy_at(&frame);
        block_ = position.block == no_block ? std::size_t{} : position.block;
        used_ = position.block == no_block ? std::size_t{} : position.used;
    }

    [[nodiscard]] void *allocate(Frame &, std::size_t size, std::size_t alignment) { return allocate(size, alignment); }

  private:
    struct Block {
        std::byte *data;
        std::size_t size;
    };

    [[nodiscard]] Position current_position() const noexcept {
        return blocks_.empty() ? Position{no_block, 0} : Position{block_, used_};
    }

    [[nodiscard]] void *allocate(std::size_t size, std::size_t alignment) {
        if (blocks_.empty()) {
            add_block(std::max(initial_block_size, size + alignment - 1));
        }

        while (true) {
            auto &block = blocks_[block_];
            auto *start = block.data + used_;
            auto *result = static_cast<void *>(start);
            auto available = block.size - used_;
            if (std::align(alignment, size, result, available) != nullptr) {
                used_ = static_cast<std::size_t>(static_cast<std::byte *>(result) - block.data) + size;
                return result;
            }

            ++block_;
            used_ = 0;
            if (block_ == blocks_.size()) {
                add_block(std::max(block.size * 2, size + alignment - 1));
            }
        }
    }

    void add_block(std::size_t size) {
        auto *data = static_cast<std::byte *>(memory_->allocate(size, alignof(std::max_align_t)));
        try {
            blocks_.push_back({data, size});
        } catch (...) {
            memory_->deallocate(data, size, alignof(std::max_align_t));
            throw;
        }
    }

    std::pmr::memory_resource *memory_;
    std::pmr::vector<Block> blocks_;
    std::size_t block_{};
    std::size_t used_{};
};

} // namespace bg::details

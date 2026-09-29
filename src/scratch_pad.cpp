#include <bg/scratch_pad.hpp>

#include <bg/details/evaluation_scratch.hpp>

namespace bg {

ScratchPad::~ScratchPad() {
    if (frame_ != nullptr) {
        auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
        frame.scratch->pop_frame(frame);
    }
}

ScratchPad::ScratchPad(ScratchPad &&other) noexcept
    : evaluation_scratch_{other.evaluation_scratch_}, frame_{other.frame_}, local_storage_{other.local_storage_} {
    for (auto *scope = local_storage_; scope != nullptr; scope = scope->previous_) {
        scope->scratch_pad_ = this;
    }

    other.frame_ = nullptr;
    other.local_storage_ = nullptr;
}

void *ScratchPad::allocate_from_storage(std::size_t size, std::size_t alignment) {
    ensure_frame();
    auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
    return evaluation_scratch_->allocate(frame, size, alignment);
}

void ScratchPad::release_last_from_storage(void *address, std::size_t size) noexcept {
    auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
    evaluation_scratch_->release_last(frame, address, size);
}

void ScratchPad::ensure_frame() {
    if (frame_ == nullptr) {
        frame_ = &evaluation_scratch_->push_frame();
    }
}

} // namespace bg

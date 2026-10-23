#include <bg/scratch_pad.hpp>

#include <bg/details/evaluation_scratch.hpp>

namespace bg {

ScratchPad::~ScratchPad() {
    reset();
}

ScratchPad::ScratchPad(ScratchPad &&other) noexcept
    : evaluation_scratch_{other.evaluation_scratch_}, frame_{other.frame_} {
    other.frame_ = nullptr;
}

void *ScratchPad::allocate(std::size_t size, std::size_t alignment) {
    ensure_frame();
    auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
    return evaluation_scratch_->allocate(frame, size, alignment);
}

void ScratchPad::release_last(void *address, std::size_t size) noexcept {
    auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
    evaluation_scratch_->release_last(frame, address, size);
}

void ScratchPad::ensure_frame() {
    if (frame_ == nullptr) {
        frame_ = &evaluation_scratch_->push_frame();
    }
}

void ScratchPad::reset() noexcept {
    if (frame_ == nullptr) {
        return;
    }

    auto &frame = *static_cast<details::EvaluationScratch::Frame *>(frame_);
    frame.scratch->pop_frame(frame);
    frame_ = nullptr;
}

} // namespace bg

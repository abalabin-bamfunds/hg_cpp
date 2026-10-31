#pragma once

#include <cstdint>

namespace bg::python {

class CallbackLifetime {
  public:
    std::uint64_t begin() noexcept {
        active_ = false;
        ++generation_;
        active_ = true;
        return generation_;
    }

    void end(std::uint64_t generation) noexcept {
        if (generation_ == generation) {
            active_ = false;
        }
    }

    bool active(std::uint64_t generation) const noexcept { return active_ && generation_ == generation; }

  private:
    std::uint64_t generation_{};
    bool active_{};
};

class CallbackScope {
  public:
    explicit CallbackScope(CallbackLifetime &lifetime) noexcept : lifetime_{&lifetime}, generation_{lifetime.begin()} {}
    ~CallbackScope() { lifetime_->end(generation_); }

    CallbackScope(const CallbackScope &) = delete;
    CallbackScope &operator=(const CallbackScope &) = delete;

    std::uint64_t generation() const noexcept { return generation_; }

  private:
    CallbackLifetime *lifetime_;
    std::uint64_t generation_;
};

} // namespace bg::python

#pragma once

#include <bg/node/input.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <cstdint>
#include <memory>
#include <stdexcept>

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

class NB_HIDDEN InputView {
  public:
    InputView(In<TS<SCALAR>> input, const PythonScalarConversions &conversions,
              std::shared_ptr<CallbackLifetime> lifetime,
              std::uint64_t generation) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation} {}

    bool valid() const {
        check_lifetime();
        return input_.valid();
    }

    bool modified() const {
        check_lifetime();
        return input_.modified();
    }

    nanobind::object value() const {
        check_lifetime();
        if (!input_.valid()) {
            throw std::runtime_error{"an invalid Python time-series input has no value"};
        }
        const auto value = input_.value_view();
        return conversions_->to_python(Scalar<>::from(value));
    }

  private:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python time-series input view was used outside its callback"};
        }
    }

    In<TS<SCALAR>> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

} // namespace bg::python

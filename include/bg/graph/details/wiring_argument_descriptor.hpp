#pragma once

#include <bg/graph/wiring_argument.hpp>

#include <optional>

namespace bg::details {

// Describes one flat argument exposed to wiring-time resolvers and requirements.
// A direct collection binding can describe its items without creating item wiring records.
class WiringArgumentDescriptor {
  public:
    explicit WiringArgumentDescriptor(const WiringArgument &argument) noexcept
        : argument_{&argument}, name_{argument.name()}, type_{&argument.type()}, present_{argument.present()},
          scalar_{argument.is_scalar()} {}

    WiringArgumentDescriptor(util::symbol name, const TimeSeriesType &type) noexcept
        : name_{name}, type_{&type}, present_{true}, scalar_{false} {}

    bool is_time_series() const noexcept { return !scalar_; }
    bool is_scalar() const noexcept { return scalar_; }
    bool present() const noexcept { return present_; }
    std::optional<util::symbol> name() const noexcept { return name_; }
    const Type &type() const noexcept { return *type_; }

    std::optional<Scalar<>> scalar() const {
        if (argument_ == nullptr || !scalar_ || !present_) {
            return std::nullopt;
        }
        return argument_->scalar();
    }

  private:
    const WiringArgument *argument_{};
    std::optional<util::symbol> name_;
    const Type *type_;
    bool present_;
    bool scalar_;
};

} // namespace bg::details

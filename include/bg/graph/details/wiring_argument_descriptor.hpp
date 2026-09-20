#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/graph/wiring_argument.hpp>

#include <optional>

namespace bg::details {

// Describes one flat argument exposed to wiring-time resolvers and requirements.
// A direct collection binding can describe its items without creating item wiring records.
class WiringArgumentDescriptor {
  public:
    explicit WiringArgumentDescriptor(const WiringArgument &argument,
                                      VariadicMode variadic_mode = VariadicMode::none) noexcept
        : argument_{&argument}, name_{argument.name()}, type_{&argument.type()}, present_{argument.present()},
          scalar_{argument.is_scalar()}, variadic_mode_{variadic_mode} {}

    WiringArgumentDescriptor(util::symbol name, const TimeSeriesType &type, VariadicMode variadic_mode) noexcept
        : name_{name}, type_{&type}, present_{true}, scalar_{false}, variadic_mode_{variadic_mode} {}

    bool is_time_series() const noexcept { return !scalar_; }
    bool is_scalar() const noexcept { return scalar_; }
    bool present() const noexcept { return present_; }
    std::optional<util::symbol> name() const noexcept { return name_; }
    const Type &type() const noexcept { return *type_; }
    VariadicMode variadic_mode() const noexcept { return variadic_mode_; }

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
    VariadicMode variadic_mode_;
};

} // namespace bg::details

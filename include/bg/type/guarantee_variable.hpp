#pragma once

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <optional>

namespace bg {

class GuaranteeVariable final : public Variable {
  public:
    explicit GuaranteeVariable(util::symbol name, std::optional<TimeSeriesGuarantee> minimum = std::nullopt)
        : Variable(name), minimum_(minimum) {}

    [[nodiscard]] bool match(TimeSeriesGuarantee supplied, TypeMap &bindings) const {
        if (minimum_ && !satisfies(supplied, *minimum_)) {
            return false;
        }

        if (const auto *binding = bindings.find(*this)) {
            const auto *guarantee = std::get_if<TimeSeriesGuarantee>(binding);
            if (guarantee == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                          "guarantee variable has a non-guarantee binding"};
            }

            bindings.set(*this, weakest(*guarantee, supplied));
        } else {
            bindings.set(*this, supplied);
        }

        return true;
    }

    [[nodiscard]] TimeSeriesGuarantee resolve(const TypeMap &bindings) const {
        const auto *binding = bindings.find(*this);
        if (binding == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unbound_variable, name(),
                                      "guarantee variable is unbound"};
        }

        const auto *guarantee = std::get_if<TimeSeriesGuarantee>(binding);
        if (guarantee == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_binding, name(),
                                      "guarantee variable has a non-guarantee binding"};
        }

        return *guarantee;
    }

  private:
    std::optional<TimeSeriesGuarantee> minimum_;
};

} // namespace bg

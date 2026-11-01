#pragma once

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <optional>

namespace bg {

class GuaranteeVariable final : public Variable {
  public:
    explicit GuaranteeVariable(util::symbol name, std::optional<TimeSeriesGuarantee> minimum = std::nullopt)
        : Variable(name), minimum_(minimum) {}

    [[nodiscard]] bool match(TimeSeriesGuarantee supplied, TypeMap &type_map) const {
        if (minimum_ && !satisfies(supplied, *minimum_)) {
            return false;
        }

        if (const auto *mapping = type_map.find(*this)) {
            const auto *guarantee = std::get_if<TimeSeriesGuarantee>(mapping);
            if (guarantee == nullptr) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                          "guarantee variable has a non-guarantee mapping"};
            }

            type_map.set(*this, weakest(*guarantee, supplied));
        } else {
            type_map.set(*this, supplied);
        }

        return true;
    }

    [[nodiscard]] TimeSeriesGuarantee resolve(const TypeMap &type_map) const {
        const auto *mapping = type_map.find(*this);
        if (mapping == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::unmapped_variable, name(),
                                      "guarantee variable has no type map entry"};
        }

        const auto *guarantee = std::get_if<TimeSeriesGuarantee>(mapping);
        if (guarantee == nullptr) {
            throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map, name(),
                                      "guarantee variable has a non-guarantee mapping"};
        }

        return *guarantee;
    }

    std::optional<TimeSeriesGuarantee> minimum() const noexcept { return minimum_; }

  private:
    std::optional<TimeSeriesGuarantee> minimum_;
};

} // namespace bg

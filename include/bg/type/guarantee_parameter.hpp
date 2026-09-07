#pragma once

#include <bg/type/guarantee_variable.hpp>

#include <optional>
#include <variant>

namespace bg {

class GuaranteeParameter {
  public:
    GuaranteeParameter() = default;
    GuaranteeParameter(TimeSeriesGuarantee value) : value_(value) {}
    GuaranteeParameter(const GuaranteeVariable &variable) : value_(&variable) {}

    bool is_resolved() const noexcept { return std::holds_alternative<TimeSeriesGuarantee>(value_); }

    std::optional<TimeSeriesGuarantee> value() const noexcept {
        if (const auto *value = std::get_if<TimeSeriesGuarantee>(&value_)) {
            return *value;
        }

        return std::nullopt;
    }

    const GuaranteeVariable *variable() const noexcept {
        if (const auto *variable = std::get_if<const GuaranteeVariable *>(&value_)) {
            return *variable;
        }

        return nullptr;
    }

    [[nodiscard]] bool match(TimeSeriesGuarantee supplied, TypeMap &bindings) const;
    [[nodiscard]] TimeSeriesGuarantee resolve(const TypeMap &bindings) const;

  private:
    std::variant<std::monostate, TimeSeriesGuarantee, const GuaranteeVariable *> value_;
};

} // namespace bg

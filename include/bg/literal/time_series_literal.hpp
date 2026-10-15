#pragma once

#include <bg/literal/literal.hpp>
#include <bg/literal/scalar_literal.hpp>

#include <ddv/serial_visitor.h>

#include <concepts>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace literal {

struct SkipLiteral {};
struct InvalidLiteral {};
struct RemoveLiteral {};

struct RemovedLiteral {
    const ScalarLiteral *value;
};

inline constexpr SkipLiteral SKIP{};
inline constexpr InvalidLiteral INVALID{};
inline constexpr RemoveLiteral REMOVE{};

class TimeSeriesLiteralItem {
  public:
    TimeSeriesLiteralItem(SkipLiteral marker) noexcept : value_{marker} {}
    TimeSeriesLiteralItem(InvalidLiteral marker) noexcept : value_{marker} {}
    TimeSeriesLiteralItem(RemoveLiteral marker) noexcept : value_{marker} {}
    TimeSeriesLiteralItem(RemovedLiteral marker) noexcept : value_{marker} {}

    template <typename Source>
        requires(!std::same_as<std::remove_cvref_t<Source>, TimeSeriesLiteralItem> &&
                 std::constructible_from<ScalarLiteralItem, Source &&>)
    TimeSeriesLiteralItem(Source &&source) : value_{ScalarLiteralItem{std::forward<Source>(source)}} {}

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    auto visit_scalar(Visitor &&visitor, Arguments &&...arguments) const {
        const auto *item = std::get_if<ScalarLiteralItem>(&value_);
        if (item == nullptr) {
            throw std::invalid_argument{"a time-series marker is not a scalar literal value"};
        }
        return item->visit_scalar(std::forward<Visitor>(visitor), std::forward<Arguments>(arguments)...);
    }

    template <typename Function, typename... Functions>
    decltype(auto) visit_scalar(Function &&function, Functions &&...functions) const {
        return visit_scalar(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

    template <typename Visitor, typename... Arguments>
        requires ddv::SerialVisitorType<std::remove_cvref_t<Visitor>>
    void visit_item(Visitor &&visitor, Arguments &&...arguments) const {
        std::visit(
            [&](const auto &item) {
                std::forward<Visitor>(visitor).visit(&item, std::forward<Arguments>(arguments)...);
            },
            value_);
    }

    template <typename Function, typename... Functions>
    void visit_item(Function &&function, Functions &&...functions) const {
        visit_item(ddv::serial{std::forward<Function>(function), std::forward<Functions>(functions)...});
    }

  private:
    std::variant<ScalarLiteralItem, SkipLiteral, InvalidLiteral, RemoveLiteral, RemovedLiteral> value_;
};

using TimeSeriesLiteral = Literal<TimeSeriesLiteralItem>;

inline RemovedLiteral REMOVED(const ScalarLiteral &value) noexcept {
    return {&value};
}

} // namespace literal

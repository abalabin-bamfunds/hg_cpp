#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/scalar/string_builder.hpp>
#include <bg/type/time_series_type.hpp>

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

struct add_strings {
    std::string_view lhs;
    std::string_view rhs;
    Out<TS<string, G>> output;

    static TimeSeriesGuarantee resolve(decltype(G), const WiringArguments &arguments) {
        auto guarantee = TimeSeriesGuarantee::const_;
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            const auto &type = arguments.resolved_type(index);
            if (!type.is_time_series()) {
                continue;
            }

            const auto &input = type_cast<TimeSeriesType>(type);
            guarantee = weakest(guarantee, *input.guarantee().value());
        }

        return guarantee;
    }

    void evaluate(EvaluationContext *context) const {
        if (rhs.size() > std::numeric_limits<std::size_t>::max() - lhs.size()) {
            throw std::length_error{"string concatenation exceeds the maximum size"};
        }

        auto builder = StringBuilder{context->runtime_graph().allocation(), lhs.size() + rhs.size()};
        builder.append(lhs);
        builder.append(rhs);

        auto value = std::move(builder).finish();
        output.set_value(value.view());
    }
};

struct add_optional_strings {
    std::optional<std::string_view> lhs;
    std::optional<std::string_view> rhs;
    Out<TS<string>> output;

    void evaluate(EvaluationContext *context) const {
        const auto lhs_value = lhs.value_or(std::string_view{});
        const auto rhs_value = rhs.value_or(std::string_view{});
        if (rhs_value.size() > std::numeric_limits<std::size_t>::max() - lhs_value.size()) {
            throw std::length_error{"string concatenation exceeds the maximum size"};
        }

        auto builder = StringBuilder{context->runtime_graph().allocation(), lhs_value.size() + rhs_value.size()};
        builder.append(lhs_value);
        builder.append(rhs_value);
        auto value = std::move(builder).finish();
        output.set_value(value.view());
    }
};

} // namespace

void register_string_operators() {
    add.overload(node<add_strings>());
    add.overload(node<add_optional_strings>());
    add.overload(graph("name"_n = "add_string_scalars", [](std::string_view lhs, std::string_view rhs) {
        auto builder = StringBuilder{};
        builder.reserve(lhs.size() + rhs.size());
        builder.append(lhs);
        builder.append(rhs);
        auto value = std::move(builder).finish();
        return operators::const_(value).as<TS<string, TimeSeriesGuarantee::const_>>();
    }));

    eq.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs == rhs; }));
    eq.overload(node([](symbol lhs, symbol rhs) { return lhs == rhs; }));
    ne.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs != rhs; }));
    ne.overload(node([](symbol lhs, symbol rhs) { return lhs != rhs; }));

    lt.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs < rhs; }));
    le.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs <= rhs; }));
    gt.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs > rhs; }));
    ge.overload(node([](std::string_view lhs, std::string_view rhs) { return lhs >= rhs; }));
}

} // namespace bg::operators::details

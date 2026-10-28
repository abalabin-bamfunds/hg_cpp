#pragma once

#include <bg/allocator.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/literal/time_series_literal.hpp>
#include <bg/scalar/runtime_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/util/lifetimebound.hpp>

#include <concepts>
#include <deque>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

struct GraphRunInput {
    const ScalarRuntimeType &runtime_type;
    std::span<const std::optional<ScalarValue<>>> ticks;
};

class GraphRunLiteralInput {
  private:
    struct OwnedStorage {
        std::span<const literal::TimeSeriesLiteral> copy_ticks(
            std::span<const literal::TimeSeriesLiteral> source) {
            auto values = std::vector<literal::TimeSeriesLiteral>{};
            values.reserve(source.size());
            for (const auto &value : source) {
                values.emplace_back(copy_literal(value));
            }
            const auto &stored = sequences.emplace_back(std::move(values));
            return std::span<const literal::TimeSeriesLiteral>{stored};
        }

        literal::TimeSeriesLiteral copy_literal(const literal::TimeSeriesLiteral &source) {
            return source.visit(
                             [&](const literal::ScalarLiteralItem &item) { return copy_scalar(item); },
                             [&](std::span<const literal::TimeSeriesLiteral> children) {
                                 return literal::TimeSeriesLiteral(copy_ticks(children));
                             })
                .value();
        }

        literal::TimeSeriesLiteral copy_scalar(const literal::ScalarLiteralItem &source) {
            if (const auto *sentinel = literal::Sentinel::from(source); sentinel != nullptr) {
                return literal::TimeSeriesLiteral(*sentinel);
            }

            if (const auto tagged = literal::TaggedLiteral::from(source)) {
                auto &argument = tagged_arguments.emplace_back(copy_literal(tagged->argument));
                auto &value = tagged_literals.emplace_back(literal::TaggedLiteral{tagged->sentinel, argument});
                return literal::TimeSeriesLiteral(value);
            }

            return source.visit_scalar([&](const auto *scalar) {
                auto memory = allocator{};
                auto &value = scalar_values.emplace_back(*scalar, memory);
                return literal::TimeSeriesLiteral(value);
            }).value();
        }

        std::deque<std::vector<literal::TimeSeriesLiteral>> sequences;
        std::deque<ScalarValue<>> scalar_values;
        std::deque<literal::ScalarLiteral> tagged_arguments;
        std::deque<literal::TaggedLiteral> tagged_literals;
    };

  public:
    // Tick storage and borrowed scalar sources must survive the enclosing evaluation.
    GraphRunLiteralInput(std::initializer_list<literal::TimeSeriesLiteral> ticks BG_LIFETIMEBOUND) noexcept
        : ticks_{ticks.begin(), ticks.size()} {}

    GraphRunLiteralInput(std::span<const literal::TimeSeriesLiteral> ticks BG_LIFETIMEBOUND) noexcept
        : ticks_{ticks} {}

    GraphRunLiteralInput(const TimeSeriesRuntimeType &runtime_type BG_LIFETIMEBOUND,
                         std::initializer_list<literal::TimeSeriesLiteral> ticks BG_LIFETIMEBOUND) noexcept
        : runtime_type{&runtime_type}, ticks_{ticks.begin(), ticks.size()} {}

    GraphRunLiteralInput(const TimeSeriesRuntimeType &runtime_type BG_LIFETIMEBOUND,
                         std::span<const literal::TimeSeriesLiteral> ticks BG_LIFETIMEBOUND) noexcept
        : runtime_type{&runtime_type}, ticks_{ticks} {}

    GraphRunLiteralInput(const GraphRunLiteralInput &source) : runtime_type{source.runtime_type} {
        if (source.owner_) {
            ticks_ = source.ticks_;
            owner_ = source.owner_;
            return;
        }

        auto owner = std::make_shared<OwnedStorage>();
        ticks_ = owner->copy_ticks(source.ticks_);
        owner_ = std::move(owner);
    }

    GraphRunLiteralInput &operator=(const GraphRunLiteralInput &source) {
        if (this != &source) {
            auto copy = GraphRunLiteralInput{source};
            *this = std::move(copy);
        }
        return *this;
    }

    GraphRunLiteralInput(GraphRunLiteralInput &&source) noexcept
        : runtime_type{source.runtime_type}, ticks_{source.ticks_}, owner_{std::move(source.owner_)} {
        source.ticks_ = {};
        source.runtime_type = nullptr;
    }

    GraphRunLiteralInput &operator=(GraphRunLiteralInput &&source) noexcept {
        if (this != &source) {
            ticks_ = source.ticks_;
            runtime_type = source.runtime_type;
            owner_ = std::move(source.owner_);
            source.ticks_ = {};
            source.runtime_type = nullptr;
        }
        return *this;
    }

    std::span<const literal::TimeSeriesLiteral> ticks() const noexcept { return ticks_; }

    const TimeSeriesRuntimeType *runtime_type{};

  private:
    std::span<const literal::TimeSeriesLiteral> ticks_;
    std::shared_ptr<const OwnedStorage> owner_;
};

template <std::ranges::view Range> struct GraphRunTicks {
    Range values;
};

template <std::ranges::viewable_range Range>
    requires(!std::same_as<std::remove_cvref_t<Range>, GraphRunLiteralInput>)
[[nodiscard]] auto ticks(Range &&values) {
    return GraphRunTicks<std::views::all_t<Range>>{std::views::all(std::forward<Range>(values))};
}

[[nodiscard]] inline const GraphRunLiteralInput &ticks(
    GraphRunLiteralInput &&input BG_LIFETIMEBOUND) noexcept {
    return input;
}

// Annotates the temporary descriptor in place and returns it by reference for the run_graph call.
template <typename TimeSeries>
[[nodiscard]] const GraphRunLiteralInput &input(GraphRunLiteralInput &&source BG_LIFETIMEBOUND) {
    source.runtime_type = &cpp_time_series_runtime_type<TimeSeries>();
    return source;
}

[[nodiscard]] inline const GraphRunLiteralInput &input(
    const TimeSeriesRuntimeType &runtime_type BG_LIFETIMEBOUND, GraphRunLiteralInput &&source BG_LIFETIMEBOUND) {
    source.runtime_type = &runtime_type;
    return source;
}

[[nodiscard]] inline const GraphRunLiteralInput &input(
    const TimeSeriesType &time_series_type, GraphRunLiteralInput &&source BG_LIFETIMEBOUND) {
    source.runtime_type = &resolve_time_series_runtime_type(time_series_type);
    return source;
}

} // namespace bg

#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"ANALYTICAL_G">;
inline constexpr auto RESULT_G = guarantee_variable<"EWMA_RESULT_G">;

struct analytical_overloads {
    // Difference from the previous input tick.
    overload<diff> diff_i64{graph([](wp<TS<i64>> value) -> wp<TS<i64>> {
        return wp<TS<i64>>{bg::operators::sub(value, bg::operators::lag(value))};
    })};
    overload<diff> diff_double{graph([](wp<TS<double>> value) -> wp<TS<double>> {
        return wp<TS<double>>{bg::operators::sub(value, bg::operators::lag(value))};
    })};

    // Clip a value to scalar or time-series bounds.
    overload<clip> clip_i64{[](i64 value, i64 min_, i64 max_) {
        if (min_ > max_) {
            throw std::invalid_argument{"clip minimum must not exceed maximum"};
        }

        return std::clamp(value, min_, max_);
    }};
    overload<clip> clip_double{[](double value, double min_, double max_) {
        if (min_ > max_) {
            throw std::invalid_argument{"clip minimum must not exceed maximum"};
        }

        return std::clamp(value, min_, max_);
    }};

    // Exponential weighted moving average.
    struct ewma_impl {
        In<TS<double, G>> value;
        Scalar<double> alpha;
        Scalar<i64> min_periods;
        State<double> average;
        State<i64> warmup_remaining;
        State<bool> initialized;
        Out<TS<double, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            const auto input_guarantee = *value_type.guarantee().value();
            const auto periods = scalar_cast<i64>(*arguments.scalar("min_periods")).value();
            if (periods == 0) {
                return input_guarantee;
            }

            return weakest(input_guarantee, TimeSeriesGuarantee::final);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            const auto alpha = scalar_cast<double>(*arguments.scalar("alpha")).value();
            if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
                return reject("ewma alpha must be finite and between 0 and 1");
            }

            const auto periods = scalar_cast<i64>(*arguments.scalar("min_periods")).value();
            if (periods < 0) {
                return reject("ewma min_periods must be nonnegative");
            }

            return accept;
        }

        void evaluate() const {
            if (!*initialized) {
                *average = *value;
                *warmup_remaining = min_periods.value();
                *initialized = true;
            } else {
                *average += alpha.value() * (*value - *average);
            }

            if (*warmup_remaining > 0) {
                --*warmup_remaining;
                return;
            }

            output.set(*average);
        }

        void on_invalidation() const {
            *initialized = false;
            output.invalidate();
        }
    };

    overload<ewma, ewma_impl> ewma_impl;
};

} // namespace

void register_analytical_operators() {
    register_overloads<analytical_overloads>();
}

} // namespace bg::operators::details

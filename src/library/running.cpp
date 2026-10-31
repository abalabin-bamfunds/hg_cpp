#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

#include <cstdint>
#include <type_traits>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"RUNNING_G">;
inline constexpr auto RESULT_G = guarantee_variable<"RUNNING_RESULT_G">;

struct running_overloads {
    // Running sum.
    template <typename T> struct sums_impl {
        In<TS<T, G>> value;
        Out<TS<T, G>> output;

        void evaluate() const {
            const auto next = [&] {
                if (!output.valid()) {
                    return value.value();
                }
                if constexpr (std::is_same_v<T, i64>) {
                    return static_cast<i64>(static_cast<std::uint64_t>(output.value()) +
                                            static_cast<std::uint64_t>(value.value()));
                } else {
                    return output.value() + value.value();
                }
            }();
            output.set(next);
        }
    };

    template <typename T> struct sums_reset_impl {
        In<TS<T, G>> value;
        In<SIGNAL, optional> reset;
        Out<TS<T, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.invalidate();
            }
            if (!value.modified()) {
                return;
            }

            const auto next = [&] {
                if (!output.valid()) {
                    return value.value();
                }
                if constexpr (std::is_same_v<T, i64>) {
                    return static_cast<i64>(static_cast<std::uint64_t>(output.value()) +
                                            static_cast<std::uint64_t>(value.value()));
                } else {
                    return output.value() + value.value();
                }
            }();
            output.set(next);
        }
    };

    overload<sums, sums_impl<i64>> sums_i64;
    overload<sums, sums_impl<double>> sums_double;
    overload<sums, sums_reset_impl<i64>> sums_reset_i64;
    overload<sums, sums_reset_impl<double>> sums_reset_double;

    // Running minimum.
    template <typename T> struct mins_impl {
        In<TS<T, G>> value;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return strongest(*value_type.guarantee().value(), TimeSeriesGuarantee::deduplicated);
        }

        void evaluate() const {
            if (!output.valid() || value.value() < output.value()) {
                output.set(value.value());
            }
        }
    };

    template <typename T> struct mins_reset_impl {
        In<TS<T, G>> value;
        In<SIGNAL, optional> reset;
        Out<TS<T, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.invalidate();
            }
            if (value.modified() && (!output.valid() || value.value() < output.value())) {
                output.set(value.value());
            }
        }
    };

    overload<mins, mins_impl<i64>> mins_i64;
    overload<mins, mins_impl<double>> mins_double;
    overload<mins, mins_reset_impl<i64>> mins_reset_i64;
    overload<mins, mins_reset_impl<double>> mins_reset_double;

    // Running maximum.
    template <typename T> struct maxs_impl {
        In<TS<T, G>> value;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            const auto &value_type = type_cast<TimeSeriesType>(arguments.resolved_type("value"));
            return strongest(*value_type.guarantee().value(), TimeSeriesGuarantee::deduplicated);
        }

        void evaluate() const {
            if (!output.valid() || value.value() > output.value()) {
                output.set(value.value());
            }
        }
    };

    template <typename T> struct maxs_reset_impl {
        In<TS<T, G>> value;
        In<SIGNAL, optional> reset;
        Out<TS<T, TimeSeriesGuarantee::deduplicated>> output;

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.invalidate();
            }
            if (value.modified() && (!output.valid() || value.value() > output.value())) {
                output.set(value.value());
            }
        }
    };

    overload<maxs, maxs_impl<i64>> maxs_i64;
    overload<maxs, maxs_impl<double>> maxs_double;
    overload<maxs, maxs_reset_impl<i64>> maxs_reset_i64;
    overload<maxs, maxs_reset_impl<double>> maxs_reset_double;

    // Running mean.
    template <typename T> struct means_impl {
        In<TS<T, G>> value;
        State<double> total;
        State<i64> count;
        Out<TS<double, G>> output;

        void evaluate() const {
            if (!output.valid()) {
                *total = 0.0;
                *count = 0;
            }
            *total += static_cast<double>(value.value());
            *count = static_cast<i64>(static_cast<std::uint64_t>(*count) + std::uint64_t{1});
            output.set(*total / static_cast<double>(*count));
        }
    };

    template <typename T> struct means_reset_impl {
        In<TS<T, G>> value;
        In<SIGNAL, optional> reset;
        State<double> total;
        State<i64> count;
        Out<TS<double, TimeSeriesGuarantee::ordinary>> output;

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.invalidate();
            }
            if (!value.modified()) {
                return;
            }
            if (!output.valid()) {
                *total = 0.0;
                *count = 0;
            }

            *total += static_cast<double>(value.value());
            *count = static_cast<i64>(static_cast<std::uint64_t>(*count) + std::uint64_t{1});
            output.set(*total / static_cast<double>(*count));
        }
    };

    overload<means, means_impl<i64>> means_i64;
    overload<means, means_impl<double>> means_double;
    overload<means, means_reset_impl<i64>> means_reset_i64;
    overload<means, means_reset_impl<double>> means_reset_double;
};

} // namespace

void register_running_operators() { register_overloads<running_overloads>(); }

} // namespace bg::operators::details

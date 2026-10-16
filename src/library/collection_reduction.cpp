#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_type.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"COLLECTION_REDUCTION_G">;
inline constexpr auto RESULT_G = guarantee_variable<"COLLECTION_REDUCTION_RESULT_G">;
using KEY = ScalarVariable<"COLLECTION_REDUCTION_KEY", Hashable>;

bool fixed_tsl(const WiringArguments &arguments) {
    const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
    return type_cast<TimeSeriesListType>(type.target_type()).item_count() != std::dynamic_extent;
}

TimeSeriesGuarantee result_guarantee(const WiringArguments &arguments) {
    const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
    return weakest(*type.guarantee().value(), TimeSeriesGuarantee::final);
}

template <InputValidity Validity>
TimeSeriesGuarantee sum_result_guarantee(const WiringArguments &arguments) {
    if constexpr (Validity == InputValidity::optional) {
        return TimeSeriesGuarantee::ordinary;
    } else {
        const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
        return *type.guarantee().value();
    }
}

template <typename T> struct sum_reducer {
    void add(T value) {
        if constexpr (std::same_as<T, i64>) {
            total = static_cast<i64>(static_cast<std::uint64_t>(total) + static_cast<std::uint64_t>(value));
        } else {
            total += value;
        }
    }

    T result() const { return total; }

    T total{};
};

template <typename T> struct minimum_reducer {
    void add(T value) {
        if (!minimum || value < *minimum) {
            minimum = value;
        }
    }

    std::optional<T> result() const { return minimum; }

    std::optional<T> minimum;
};

template <typename T> struct maximum_reducer {
    void add(T value) {
        if (!maximum || value > *maximum) {
            maximum = value;
        }
    }

    std::optional<T> result() const { return maximum; }

    std::optional<T> maximum;
};

template <typename T> struct mean_reducer {
    void add(T value) {
        total += static_cast<double>(value);
        ++count;
    }

    std::optional<double> result() const {
        if (count == 0) {
            return std::nullopt;
        }
        return total / static_cast<double>(count);
    }

    double total{};
    std::size_t count{};
};

template <InputValidity Validity, typename T, typename Function>
void visit_collection_values(const In<TSL<TS<T, G>, SIZE, G>, Validity> &values, Function &&function) {
    if (!values.valid()) {
        return;
    }

    const auto size = values.size();
    for (auto index = std::size_t{}; index < size; ++index) {
        const auto item = values[index];
        if constexpr (Validity == InputValidity::optional) {
            if (!item.valid()) {
                continue;
            }
        }
        function(item.value());
    }
}

template <InputValidity Validity, typename T, typename Function>
void visit_collection_values(const In<TSD<KEY, TS<T, G>, G>, Validity> &values, Function &&function) {
    if (!values.valid()) {
        return;
    }

    values.for_each_value([&](In<TS<T, G>> item) {
        if constexpr (Validity == InputValidity::optional) {
            if (!item.valid()) {
                return;
            }
        }
        function(item.value());
    });
}

template <InputValidity Validity, typename T, typename Function>
void visit_collection_values(const In<TSS<T, G>, Validity> &values, Function &&function) {
    if (!values.valid()) {
        return;
    }

    values.for_each([&](Scalar<T> item) { function(item.value()); });
}

template <typename Reducer, InputValidity Validity, typename Collection>
auto calculate_collection_value(const Collection &values) {
    auto reducer = Reducer{};
    visit_collection_values<Validity>(values, [&](auto value) { reducer.add(value); });
    return reducer.result();
}

struct collection_reduction_overloads {
    // Collection sum.
    template <typename T, InputValidity Validity> struct sum_tsl_impl {
        In<TSL<TS<T, G>, SIZE, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }
        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return sum_result_guarantee<Validity>(arguments);
        }

        void evaluate() const {
            output.set(calculate_collection_value<sum_reducer<T>, Validity>(values));
        }
    };

    template <typename T, InputValidity Validity> struct sum_tsd_impl {
        In<TSD<KEY, TS<T, G>, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return sum_result_guarantee<Validity>(arguments);
        }

        void evaluate() const {
            output.set(calculate_collection_value<sum_reducer<T>, Validity>(values));
        }
    };

    template <typename T, InputValidity Validity> struct sum_tss_impl {
        In<TSS<T, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return sum_result_guarantee<Validity>(arguments);
        }

        void evaluate() const {
            output.set(calculate_collection_value<sum_reducer<T>, Validity>(values));
        }
    };

    template <typename T> using sum_tsl_all_valid_family = sum_tsl_impl<T, InputValidity::all_valid>;
    template <typename T> using sum_tsl_optional_family = sum_tsl_impl<T, InputValidity::optional>;
    template <typename T> using sum_tsd_all_valid_family = sum_tsd_impl<T, InputValidity::all_valid>;
    template <typename T> using sum_tsd_optional_family = sum_tsd_impl<T, InputValidity::optional>;
    template <typename T> using sum_tss_all_valid_family = sum_tss_impl<T, InputValidity::all_valid>;
    template <typename T> using sum_tss_optional_family = sum_tss_impl<T, InputValidity::optional>;

    instantiate_overloads<sum_, sum_tsl_all_valid_family, i64, double> sum_tsl_all_valid;
    instantiate_overloads<sum_, sum_tsl_optional_family, i64, double> sum_tsl_optional;
    instantiate_overloads<sum_, sum_tsd_all_valid_family, i64, double> sum_tsd_all_valid;
    instantiate_overloads<sum_, sum_tsd_optional_family, i64, double> sum_tsd_optional;
    instantiate_overloads<sum_, sum_tss_all_valid_family, i64, double> sum_tss_all_valid;
    instantiate_overloads<sum_, sum_tss_optional_family, i64, double> sum_tss_optional;

    // Collection minimum.
    template <typename T, InputValidity Validity> struct min_tsl_impl {
        In<TSL<TS<T, G>, SIZE, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }
        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto minimum = calculate_collection_value<minimum_reducer<T>, Validity>(values);
            if (minimum) {
                output.set(*minimum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct min_tsd_impl {
        In<TSD<KEY, TS<T, G>, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto minimum = calculate_collection_value<minimum_reducer<T>, Validity>(values);
            if (minimum) {
                output.set(*minimum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct min_tss_impl {
        In<TSS<T, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto minimum = calculate_collection_value<minimum_reducer<T>, Validity>(values);
            if (minimum) {
                output.set(*minimum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T> using min_tsl_all_valid_family = min_tsl_impl<T, InputValidity::all_valid>;
    template <typename T> using min_tsl_optional_family = min_tsl_impl<T, InputValidity::optional>;
    template <typename T> using min_tsd_all_valid_family = min_tsd_impl<T, InputValidity::all_valid>;
    template <typename T> using min_tsd_optional_family = min_tsd_impl<T, InputValidity::optional>;
    template <typename T> using min_tss_all_valid_family = min_tss_impl<T, InputValidity::all_valid>;
    template <typename T> using min_tss_optional_family = min_tss_impl<T, InputValidity::optional>;

    instantiate_overloads<min_, min_tsl_all_valid_family, i64, double> min_tsl_all_valid;
    instantiate_overloads<min_, min_tsl_optional_family, i64, double> min_tsl_optional;
    instantiate_overloads<min_, min_tsd_all_valid_family, i64, double> min_tsd_all_valid;
    instantiate_overloads<min_, min_tsd_optional_family, i64, double> min_tsd_optional;
    instantiate_overloads<min_, min_tss_all_valid_family, i64, double> min_tss_all_valid;
    instantiate_overloads<min_, min_tss_optional_family, i64, double> min_tss_optional;

    // Collection maximum.
    template <typename T, InputValidity Validity> struct max_tsl_impl {
        In<TSL<TS<T, G>, SIZE, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }
        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto maximum = calculate_collection_value<maximum_reducer<T>, Validity>(values);
            if (maximum) {
                output.set(*maximum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct max_tsd_impl {
        In<TSD<KEY, TS<T, G>, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto maximum = calculate_collection_value<maximum_reducer<T>, Validity>(values);
            if (maximum) {
                output.set(*maximum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct max_tss_impl {
        In<TSS<T, G>, Validity> values;
        Out<TS<T, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto maximum = calculate_collection_value<maximum_reducer<T>, Validity>(values);
            if (maximum) {
                output.set(*maximum);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T> using max_tsl_all_valid_family = max_tsl_impl<T, InputValidity::all_valid>;
    template <typename T> using max_tsl_optional_family = max_tsl_impl<T, InputValidity::optional>;
    template <typename T> using max_tsd_all_valid_family = max_tsd_impl<T, InputValidity::all_valid>;
    template <typename T> using max_tsd_optional_family = max_tsd_impl<T, InputValidity::optional>;
    template <typename T> using max_tss_all_valid_family = max_tss_impl<T, InputValidity::all_valid>;
    template <typename T> using max_tss_optional_family = max_tss_impl<T, InputValidity::optional>;

    instantiate_overloads<max_, max_tsl_all_valid_family, i64, double> max_tsl_all_valid;
    instantiate_overloads<max_, max_tsl_optional_family, i64, double> max_tsl_optional;
    instantiate_overloads<max_, max_tsd_all_valid_family, i64, double> max_tsd_all_valid;
    instantiate_overloads<max_, max_tsd_optional_family, i64, double> max_tsd_optional;
    instantiate_overloads<max_, max_tss_all_valid_family, i64, double> max_tss_all_valid;
    instantiate_overloads<max_, max_tss_optional_family, i64, double> max_tss_optional;

    // Collection mean.
    template <typename T, InputValidity Validity> struct mean_tsl_impl {
        In<TSL<TS<T, G>, SIZE, G>, Validity> values;
        Out<TS<double, RESULT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }
        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto mean = calculate_collection_value<mean_reducer<T>, Validity>(values);
            if (mean) {
                output.set(*mean);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct mean_tsd_impl {
        In<TSD<KEY, TS<T, G>, G>, Validity> values;
        Out<TS<double, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto mean = calculate_collection_value<mean_reducer<T>, Validity>(values);
            if (mean) {
                output.set(*mean);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T, InputValidity Validity> struct mean_tss_impl {
        In<TSS<T, G>, Validity> values;
        Out<TS<double, RESULT_G>> output;

        static TimeSeriesGuarantee resolve(decltype(RESULT_G), const WiringArguments &arguments) {
            return result_guarantee(arguments);
        }

        void evaluate() const {
            const auto mean = calculate_collection_value<mean_reducer<T>, Validity>(values);
            if (mean) {
                output.set(*mean);
                return;
            }
            output.invalidate();
        }
    };

    template <typename T> using mean_tsl_all_valid_family = mean_tsl_impl<T, InputValidity::all_valid>;
    template <typename T> using mean_tsl_optional_family = mean_tsl_impl<T, InputValidity::optional>;
    template <typename T> using mean_tsd_all_valid_family = mean_tsd_impl<T, InputValidity::all_valid>;
    template <typename T> using mean_tsd_optional_family = mean_tsd_impl<T, InputValidity::optional>;
    template <typename T> using mean_tss_all_valid_family = mean_tss_impl<T, InputValidity::all_valid>;
    template <typename T> using mean_tss_optional_family = mean_tss_impl<T, InputValidity::optional>;

    instantiate_overloads<mean, mean_tsl_all_valid_family, i64, double> mean_tsl_all_valid;
    instantiate_overloads<mean, mean_tsl_optional_family, i64, double> mean_tsl_optional;
    instantiate_overloads<mean, mean_tsd_all_valid_family, i64, double> mean_tsd_all_valid;
    instantiate_overloads<mean, mean_tsd_optional_family, i64, double> mean_tsd_optional;
    instantiate_overloads<mean, mean_tss_all_valid_family, i64, double> mean_tss_all_valid;
    instantiate_overloads<mean, mean_tss_optional_family, i64, double> mean_tss_optional;
};

} // namespace

void register_collection_reduction_operators() { register_overloads<collection_reduction_overloads>(); }

} // namespace bg::operators::details

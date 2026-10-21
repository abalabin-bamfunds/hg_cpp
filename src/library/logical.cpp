#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

namespace bg::operators::details {

namespace {

inline constexpr auto G = guarantee_variable<"BOOLEAN_REDUCTION_G">;
using KEY = ScalarVariable<"BOOLEAN_REDUCTION_KEY", Hashable>;

bool fixed_tsl(const WiringArguments &arguments) {
    const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type("values"));
    return type_cast<TimeSeriesListType>(type.target_type()).item_count() != std::dynamic_extent;
}

struct logical_overloads {
    // Equality.
    overload<eq> eq_bool{[](bool lhs, bool rhs) { return lhs == rhs; }};
    overload<eq> eq_byte{[](byte lhs, byte rhs) { return lhs == rhs; }};
    overload<eq> eq_impl{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return lhs.value_view().equals(rhs.value_view());
    }};

    // Inequality.
    overload<ne> ne_bool{[](bool lhs, bool rhs) { return lhs != rhs; }};
    overload<ne> ne_byte{[](byte lhs, byte rhs) { return lhs != rhs; }};
    overload<ne> ne_impl{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return !lhs.value_view().equals(rhs.value_view());
    }};

    // Boolean logic.
    overload<and_> and_impl{[](bool lhs, bool rhs) { return lhs && rhs; }};
    overload<or_> or_impl{[](bool lhs, bool rhs) { return lhs || rhs; }};
    overload<not_> not_impl{[](bool value) { return !value; }};

    // all_
    template <InputValidity Validity, auto OUTPUT_G> struct all_tsl_impl {
        In<TSL<TS<bool, G>, SIZE, G>, Validity> values;
        Out<TS<bool, OUTPUT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }

        void evaluate() const {
            if constexpr (Validity == optional) {
                if (!values.valid()) {
                    output.set(true);
                    return;
                }
            }
            const auto size = values.size();
            for (auto index = std::size_t{}; index < size; ++index) {
                const auto item = values[index];
                if constexpr (Validity == optional) {
                    if (!item.valid()) {
                        continue;
                    }
                }
                if (!item.value()) {
                    output.set(false);
                    return;
                }
            }
            output.set(true);
        }
    };

    overload<all_, all_tsl_impl<InputValidity::all_valid, G>> all_tsl_required;
    overload<all_, all_tsl_impl<optional, TimeSeriesGuarantee::ordinary>> all_tsl_optional;

    template <InputValidity Validity, auto OUTPUT_G, auto... PROPERTIES> struct all_tsd_impl {
        In<TSD<KEY, TS<bool, G>, G>, Validity, PROPERTIES...> values;
        Out<TS<bool, OUTPUT_G>> output;

        void evaluate() const {
            if constexpr (Validity == optional) {
                if (!values.valid()) {
                    output.set(true);
                    return;
                }
            }

            auto result = true;
            values.for_each_value([&](auto item) {
                if constexpr (Validity == optional && !decltype(values)::valid_items) {
                    if (!item.valid()) {
                        return;
                    }
                }
                result = result && item.value();
            });
            output.set(result);
        }
    };

    overload<all_, all_tsd_impl<InputValidity::all_valid, G>> all_tsd_required;
    overload<all_, all_tsd_impl<optional, TimeSeriesGuarantee::ordinary>> all_tsd_optional;
    overload<all_, all_tsd_impl<InputValidity::all_valid, G, valid_items>> all_tsd_valid_items_required;
    overload<all_, all_tsd_impl<optional, TimeSeriesGuarantee::ordinary, valid_items>> all_tsd_valid_items_optional;

    template <InputValidity Validity, auto OUTPUT_G> struct all_tss_impl {
        In<TSS<bool, G>, Validity> values;
        Out<TS<bool, OUTPUT_G>> output;

        void evaluate() const { output.set(!values.contains(false)); }
    };

    overload<all_, all_tss_impl<InputValidity::all_valid, G>> all_tss_required;
    overload<all_, all_tss_impl<optional, TimeSeriesGuarantee::ordinary>> all_tss_optional;

    // any_
    template <InputValidity Validity, auto OUTPUT_G> struct any_tsl_impl {
        In<TSL<TS<bool, G>, SIZE, G>, Validity> values;
        Out<TS<bool, OUTPUT_G>> output;

        static bool require(const WiringArguments &arguments) { return fixed_tsl(arguments); }

        void evaluate() const {
            if constexpr (Validity == optional) {
                if (!values.valid()) {
                    output.set(false);
                    return;
                }
            }
            const auto size = values.size();
            for (auto index = std::size_t{}; index < size; ++index) {
                const auto item = values[index];
                if constexpr (Validity == optional) {
                    if (!item.valid()) {
                        continue;
                    }
                }
                if (item.value()) {
                    output.set(true);
                    return;
                }
            }
            output.set(false);
        }
    };

    overload<any_, any_tsl_impl<InputValidity::all_valid, G>> any_tsl_required;
    overload<any_, any_tsl_impl<optional, TimeSeriesGuarantee::ordinary>> any_tsl_optional;

    template <InputValidity Validity, auto OUTPUT_G, auto... PROPERTIES> struct any_tsd_impl {
        In<TSD<KEY, TS<bool, G>, G>, Validity, PROPERTIES...> values;
        Out<TS<bool, OUTPUT_G>> output;

        void evaluate() const {
            if constexpr (Validity == optional) {
                if (!values.valid()) {
                    output.set(false);
                    return;
                }
            }

            auto result = false;
            values.for_each_value([&](auto item) {
                if constexpr (Validity == optional && !decltype(values)::valid_items) {
                    if (!item.valid()) {
                        return;
                    }
                }
                result = result || item.value();
            });
            output.set(result);
        }
    };

    overload<any_, any_tsd_impl<InputValidity::all_valid, G>> any_tsd_required;
    overload<any_, any_tsd_impl<optional, TimeSeriesGuarantee::ordinary>> any_tsd_optional;
    overload<any_, any_tsd_impl<InputValidity::all_valid, G, valid_items>> any_tsd_valid_items_required;
    overload<any_, any_tsd_impl<optional, TimeSeriesGuarantee::ordinary, valid_items>> any_tsd_valid_items_optional;

    template <InputValidity Validity, auto OUTPUT_G> struct any_tss_impl {
        In<TSS<bool, G>, Validity> values;
        Out<TS<bool, OUTPUT_G>> output;

        void evaluate() const { output.set(values.contains(true)); }
    };

    overload<any_, any_tss_impl<InputValidity::all_valid, G>> any_tss_required;
    overload<any_, any_tss_impl<optional, TimeSeriesGuarantee::ordinary>> any_tss_optional;
};

} // namespace

void register_logical_operators() {
    register_overloads<logical_overloads>();
}

} // namespace bg::operators::details

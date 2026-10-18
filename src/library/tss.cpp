#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

#include <cstddef>
#include <functional>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

struct difference_contains {
    bool operator()(bool lhs, bool rhs) const noexcept { return lhs && !rhs; }
};

template <InputValidity VALIDITY, auto OUTPUT_GUARANTEE, typename PREDICATE>
struct tss_set_operation_impl {
    In<TSS<K, G>, VALIDITY> lhs;
    In<TSS<K, G>, VALIDITY> rhs;
    State<bool> needs_reconcile;
    Out<TSS<K, OUTPUT_GUARANTEE>> output;

    void evaluate(EvaluationContext *context) const {
        const auto now = context->evaluation_time();
        const auto lhs_validity_changed = lhs.last_updated() == now && (lhs.became_valid() || lhs.became_invalid());
        const auto rhs_validity_changed = rhs.last_updated() == now && (rhs.became_valid() || rhs.became_invalid());
        if (*needs_reconcile || !output.valid() || lhs_validity_changed || rhs_validity_changed) {
            reconcile();
            *needs_reconcile = false;
            return;
        }

        const auto update = [&](const auto &key) {
            if (PREDICATE{}(lhs.contains(key), rhs.contains(key))) {
                output.insert(key);
            } else {
                output.erase(key);
            }
        };

        if (lhs.last_updated() == now && lhs.modified()) {
            lhs.for_each_added(update);
            lhs.for_each_removed(update);
        }
        if (rhs.last_updated() == now && rhs.modified()) {
            rhs.for_each_added(update);
            rhs.for_each_removed(update);
        }
    }

    void on_invalidation() const { *needs_reconcile = true; }

    void reconcile() const {
        if (!output.valid()) {
            output.clear();
        }
        output.for_each([&](const auto &key) {
            if (!PREDICATE{}(lhs.contains(key), rhs.contains(key))) {
                output.erase(key);
            }
        });
        const auto add_result_members = [&](const auto &key) {
            if (PREDICATE{}(lhs.contains(key), rhs.contains(key))) {
                output.insert(key);
            }
        };
        lhs.for_each(add_result_members);
        rhs.for_each(add_result_members);
    }
};

template <InputValidity VALIDITY, auto OUTPUT_GUARANTEE, bool NEGATED>
struct tss_equality_impl {
    In<TSS<K, G>, VALIDITY> lhs;
    In<TSS<K, G>, VALIDITY> rhs;
    State<std::size_t> mismatches;
    Out<TS<bool, OUTPUT_GUARANTEE>> output;

    void evaluate(EvaluationContext *context) const {
        const auto now = context->evaluation_time();
        const auto lhs_validity_changed = lhs.last_updated() == now && (lhs.became_valid() || lhs.became_invalid());
        const auto rhs_validity_changed = rhs.last_updated() == now && (rhs.became_valid() || rhs.became_invalid());
        if (!output.valid() || lhs_validity_changed || rhs_validity_changed) {
            *mismatches = count_mismatches();
        } else {
            apply_current_changes(now);
        }

        const auto equal = *mismatches == 0;
        output.set(NEGATED ? !equal : equal);
    }

    void apply_current_changes(EvaluationTime now) const {
        const auto changed_this_cycle = [&](const auto &input, const auto &key) {
            return input.last_updated() == now && input.modified() && (input.was_added(key) || input.was_removed(key));
        };
        const auto was_present_before = [&](const auto &input, const auto &key) {
            if (!input.valid()) {
                return false;
            }
            if (!changed_this_cycle(input, key)) {
                return input.contains(key);
            }
            if (input.was_added(key)) {
                return false;
            }
            if (input.was_removed(key)) {
                return true;
            }
            return input.contains(key);
        };

        const auto update = [&](const auto &key) {
            const auto differed_before = was_present_before(lhs, key) != was_present_before(rhs, key);
            const auto differs_now = lhs.contains(key) != rhs.contains(key);
            if (differed_before == differs_now) {
                return;
            }
            if (differs_now) {
                ++*mismatches;
            } else {
                --*mismatches;
            }
        };

        const auto update_lhs = [&](const auto &key) {
            if (changed_this_cycle(rhs, key)) {
                return;
            }
            update(key);
        };
        if (lhs.last_updated() == now && lhs.modified()) {
            lhs.for_each_added(update_lhs);
            lhs.for_each_removed(update_lhs);
        }
        if (rhs.last_updated() == now && rhs.modified()) {
            rhs.for_each_added(update);
            rhs.for_each_removed(update);
        }
    }

    std::size_t count_mismatches() const {
        auto count = std::size_t{};
        lhs.for_each([&](const auto &key) {
            if (!rhs.contains(key)) {
                ++count;
            }
        });
        rhs.for_each([&](const auto &key) {
            if (!lhs.contains(key)) {
                ++count;
            }
        });
        return count;
    }
};

struct tss_overloads {
    overload<is_empty> is_empty_tss{[](In<TSS<K, G>> value, Out<TS<bool, G>> output) {
        output.set(value.size() == 0);
    }};
    overload<is_empty> is_empty_tss_optional{[](In<TSS<K, G>, optional> value) { return value.size() == 0; }};

    // TSS set equality.
    overload<eq, tss_equality_impl<required, G, false>> eq_tss;
    overload<eq, tss_equality_impl<optional, TimeSeriesGuarantee::ordinary, false>> eq_tss_optional;
    overload<ne, tss_equality_impl<required, G, true>> ne_tss;
    overload<ne, tss_equality_impl<optional, TimeSeriesGuarantee::ordinary, true>> ne_tss_optional;

    // TSS set algebra.
    overload<bit_or, tss_set_operation_impl<required, G, std::logical_or<>>> bit_or_tss;
    overload<bit_or, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, std::logical_or<>>>
        bit_or_tss_optional;
    overload<bit_and, tss_set_operation_impl<required, G, std::logical_and<>>> bit_and_tss;
    overload<bit_and, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, std::logical_and<>>>
        bit_and_tss_optional;
    overload<sub, tss_set_operation_impl<required, G, difference_contains>> sub_tss;
    overload<sub, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, difference_contains>>
        sub_tss_optional;
    overload<bit_xor, tss_set_operation_impl<required, G, std::not_equal_to<>>> bit_xor_tss;
    overload<bit_xor, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, std::not_equal_to<>>>
        bit_xor_tss_optional;
};

} // namespace

void register_tss_operators() { register_overloads<tss_overloads>(); }

} // namespace bg::operators::details

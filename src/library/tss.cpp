#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

#include <cstddef>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;

template <InputValidity VALIDITY, auto OUTPUT_GUARANTEE, typename OPERATION>
struct tss_set_operation_impl {
    In<TSS<K, G>, VALIDITY> lhs;
    In<TSS<K, G>, VALIDITY> rhs;
    State<bool> needs_reconcile;
    Out<TSS<K, OUTPUT_GUARANTEE>> output;

    void evaluate() const {
        const auto lhs_validity_changed = lhs.became_valid() || lhs.became_invalid();
        const auto rhs_validity_changed = rhs.became_valid() || rhs.became_invalid();
        if (*needs_reconcile || !output.valid() || lhs_validity_changed || rhs_validity_changed) {
            if (!output.valid()) {
                output.clear();
            }
            OPERATION::reconcile(lhs, rhs, output);
            *needs_reconcile = false;
            return;
        }

        OPERATION::update(lhs, rhs, output);
    }

    void on_invalidation() const { *needs_reconcile = true; }
};

template <InputValidity VALIDITY, auto OUTPUT_GUARANTEE, bool NEGATED>
struct tss_equality_impl {
    In<TSS<K, G>, VALIDITY> lhs;
    In<TSS<K, G>, VALIDITY> rhs;
    State<std::size_t> mismatches;
    Out<TS<bool, OUTPUT_GUARANTEE>> output;

    void evaluate() const {
        const auto lhs_validity_changed = lhs.became_valid() || lhs.became_invalid();
        const auto rhs_validity_changed = rhs.became_valid() || rhs.became_invalid();
        if (!output.valid() || lhs_validity_changed || rhs_validity_changed) {
            *mismatches = count_mismatches();
        } else {
            apply_current_changes();
        }

        const auto equal = *mismatches == 0;
        output.set(NEGATED ? !equal : equal);
    }

    void apply_current_changes() const {
        const auto apply_changes = [&](const auto &input, const auto &other) {
            const auto update = [&](const auto &key, bool added) {
                if (other.modified() && (other.was_added(key) || other.was_removed(key))) {
                    return;
                }
                if (added == other.contains(key)) {
                    --*mismatches;
                } else {
                    ++*mismatches;
                }
            };
            input.for_each_added([&](const auto &key) { update(key, true); });
            input.for_each_removed([&](const auto &key) { update(key, false); });
        };

        if (lhs.modified()) {
            apply_changes(lhs, rhs);
        }
        if (rhs.modified()) {
            apply_changes(rhs, lhs);
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

    struct keys_where_true_impl {
        In<TSD<K, TS<bool>, G>, optional> mask;
        Out<TSS<K>> output;

        void evaluate() const {
            if (!mask.valid()) {
                if (output.valid()) {
                    output.clear();
                }
                return;
            }

            if (!output.valid() || mask.became_valid()) {
                output.clear();
                mask.for_each_key([&](const auto &key) { update(key); });
                return;
            }

            mask.for_each_added([&](const auto &key) { update(key); });
            mask.for_each_removed([&](const auto &key) { output.erase(key); });
            mask.for_each_modified([&](const auto &key) { update(key); });
        }

        void update(const auto &key) const {
            const auto item = mask[key];
            if (item.valid() && item.value()) {
                output.insert(key);
            } else {
                output.erase(key);
            }
        }
    };

    overload<keys_where_true, keys_where_true_impl> keys_where_true_tsd;

    // TSS set equality.
    overload<eq, tss_equality_impl<required, G, false>> eq_tss;
    overload<eq, tss_equality_impl<optional, TimeSeriesGuarantee::ordinary, false>> eq_tss_optional;
    overload<ne, tss_equality_impl<required, G, true>> ne_tss;
    overload<ne, tss_equality_impl<optional, TimeSeriesGuarantee::ordinary, true>> ne_tss_optional;

    // TSS set algebra.
    struct tss_union {
        static void update(const auto &lhs, const auto &rhs, const auto &output) {
            const auto apply_changes = [&](const auto &input, const auto &other) {
                if (!input.modified()) {
                    return;
                }
                input.for_each_added([&](const auto &key) { output.insert(key); });
                input.for_each_removed([&](const auto &key) {
                    if (!other.contains(key)) {
                        output.erase(key);
                    }
                });
            };
            apply_changes(lhs, rhs);
            apply_changes(rhs, lhs);
        }

        static void reconcile(const auto &lhs, const auto &rhs, const auto &output) {
            output.for_each([&](const auto &key) {
                if (!lhs.contains(key) && !rhs.contains(key)) {
                    output.erase(key);
                }
            });
            lhs.for_each([&](const auto &key) { output.insert(key); });
            rhs.for_each([&](const auto &key) { output.insert(key); });
        }
    };
    overload<bit_or, tss_set_operation_impl<required, G, tss_union>> bit_or_tss;
    overload<bit_or, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, tss_union>> bit_or_tss_optional;

    struct tss_intersection {
        static void update(const auto &lhs, const auto &rhs, const auto &output) {
            const auto apply_changes = [&](const auto &input, const auto &other) {
                if (!input.modified()) {
                    return;
                }
                input.for_each_added([&](const auto &key) {
                    if (other.contains(key)) {
                        output.insert(key);
                    }
                });
                input.for_each_removed([&](const auto &key) { output.erase(key); });
            };
            apply_changes(lhs, rhs);
            apply_changes(rhs, lhs);
        }

        static void reconcile(const auto &lhs, const auto &rhs, const auto &output) {
            output.for_each([&](const auto &key) {
                if (!lhs.contains(key) || !rhs.contains(key)) {
                    output.erase(key);
                }
            });
            if (lhs.size() <= rhs.size()) {
                lhs.for_each([&](const auto &key) {
                    if (rhs.contains(key)) {
                        output.insert(key);
                    }
                });
            } else {
                rhs.for_each([&](const auto &key) {
                    if (lhs.contains(key)) {
                        output.insert(key);
                    }
                });
            }
        }
    };
    overload<bit_and, tss_set_operation_impl<required, G, tss_intersection>> bit_and_tss;
    overload<bit_and, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, tss_intersection>>
        bit_and_tss_optional;

    struct tss_difference {
        static void update(const auto &lhs, const auto &rhs, const auto &output) {
            if (lhs.modified()) {
                lhs.for_each_added([&](const auto &key) {
                    if (!rhs.contains(key)) {
                        output.insert(key);
                    }
                });
                lhs.for_each_removed([&](const auto &key) { output.erase(key); });
            }
            if (rhs.modified()) {
                rhs.for_each_added([&](const auto &key) { output.erase(key); });
                rhs.for_each_removed([&](const auto &key) {
                    if (lhs.contains(key)) {
                        output.insert(key);
                    }
                });
            }
        }

        static void reconcile(const auto &lhs, const auto &rhs, const auto &output) {
            output.for_each([&](const auto &key) {
                if (!lhs.contains(key) || rhs.contains(key)) {
                    output.erase(key);
                }
            });
            lhs.for_each([&](const auto &key) {
                if (!rhs.contains(key)) {
                    output.insert(key);
                }
            });
        }
    };
    overload<sub, tss_set_operation_impl<required, G, tss_difference>> sub_tss;
    overload<sub, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, tss_difference>> sub_tss_optional;

    struct tss_symmetric_difference {
        static void update(const auto &lhs, const auto &rhs, const auto &output) {
            const auto apply_changes = [&](const auto &input, const auto &other) {
                if (!input.modified()) {
                    return;
                }
                input.for_each_added([&](const auto &key) {
                    if (other.contains(key)) {
                        output.erase(key);
                    } else {
                        output.insert(key);
                    }
                });
                input.for_each_removed([&](const auto &key) {
                    if (other.contains(key)) {
                        output.insert(key);
                    } else {
                        output.erase(key);
                    }
                });
            };
            apply_changes(lhs, rhs);
            apply_changes(rhs, lhs);
        }

        static void reconcile(const auto &lhs, const auto &rhs, const auto &output) {
            output.for_each([&](const auto &key) {
                if (lhs.contains(key) == rhs.contains(key)) {
                    output.erase(key);
                }
            });
            lhs.for_each([&](const auto &key) {
                if (!rhs.contains(key)) {
                    output.insert(key);
                }
            });
            rhs.for_each([&](const auto &key) {
                if (!lhs.contains(key)) {
                    output.insert(key);
                }
            });
        }
    };
    overload<bit_xor, tss_set_operation_impl<required, G, tss_symmetric_difference>> bit_xor_tss;
    overload<bit_xor, tss_set_operation_impl<optional, TimeSeriesGuarantee::ordinary, tss_symmetric_difference>>
        bit_xor_tss_optional;
};

} // namespace

void register_tss_operators() { register_overloads<tss_overloads>(); }

} // namespace bg::operators::details

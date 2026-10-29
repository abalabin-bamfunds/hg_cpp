#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/type/time_series_dict_type.hpp>

#include <cstddef>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;
inline constexpr auto GD = guarantee_variable<"GD">;
inline constexpr auto GR = guarantee_variable<"GR">;

template <InputValidity VALIDITY, auto OUTPUT_GUARANTEE, typename OPERATION>
struct tss_set_operation_impl {
    In<TSS<K, G>, VALIDITY> lhs;
    In<TSS<K, G>, VALIDITY> rhs;
    Out<TSS<K, OUTPUT_GUARANTEE>> output;

    void evaluate() const {
        const auto lhs_validity_changed = lhs.became_valid() || lhs.became_invalid();
        const auto rhs_validity_changed = rhs.became_valid() || rhs.became_invalid();
        if (!output.valid() || lhs_validity_changed || rhs_validity_changed) {
            if (!output.valid()) {
                output.clear();
            }
            OPERATION::reconcile(lhs, rhs, output);
            return;
        }

        OPERATION::update(lhs, rhs, output);
    }
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

            mask.for_each_modified([&](const auto &key) { update(key); });
            mask.for_each_removed([&](const auto &key) { output.erase(key); });
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

    overload<keys_> keys_tsd{graph([](wp<TSD<K, TS_T, G>> dictionary) {
        const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary.type().target_type());
        auto &wiring = WiringContext::current().wiring();
        const auto key_set = wiring.output_item(dictionary, key_set_item_id, type.key_set_type());
        return key_set.as<TSS<K, G>>();
    })};

    struct keys_tsd_valid_items_impl {
        In<TSD<K, TS_T, G>, valid_items> dictionary;
        Out<TSS<K>> output;

        void evaluate() const {
            if (!output.valid() || dictionary.became_valid()) {
                output.clear();
                dictionary.for_each_key([&](const auto &key) { output.insert(key); });
                return;
            }

            dictionary.for_each_added_key([&](const auto &key) { output.insert(key); });
            dictionary.for_each_removed_key([&](const auto &key) { output.erase(key); });
        }
    };
    overload<keys_, keys_tsd_valid_items_impl> keys_tsd_valid_items;

    overload<len_> len_tsd_valid_items{graph([](wp<TSD<K, TS_T, G>, valid_items> dictionary) -> wp<TS<i64>> {
        return operators::len_(operators::keys_(dictionary)).template as<TS<i64>>();
    })};

    overload<is_empty> is_empty_tsd_valid_items{graph(
        [](wp<TSD<K, TS_T, G>, valid_items> dictionary) -> wp<TS<bool>> {
            return operators::is_empty(operators::keys_(dictionary)).template as<TS<bool>>();
        })};

    overload<len_> len_tsd{graph([](wp<TSD<K, TS_T, G>> dictionary) -> wp<TS<i64, G>> {
        return operators::len_(operators::keys_(dictionary)).template as<TS<i64, G>>();
    })};

    overload<is_empty> is_empty_tsd{graph([](wp<TSD<K, TS_T, G>> dictionary) -> wp<TS<bool, G>> {
        return operators::is_empty(operators::keys_(dictionary)).template as<TS<bool, G>>();
    })};

    struct keys_tsd_reference_impl {
        In<REF<TSD<K, TS_T, GD>, GR>, optional> dictionary;
        In<REF<TSS<K, GD>, GR>, dynamic_binding, optional> key_set;
        Out<REF<TSS<K, GD>, GR>> output;

        void evaluate() const {
            if (!dictionary.valid()) {
                key_set.unbind();
                output.invalidate();
                return;
            }

            key_set.bind(dictionary.key_set());
            output.copy(key_set);
        }
    };
    overload<keys_, keys_tsd_reference_impl> keys_tsd_reference;

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

struct tsd_set_overloads {
    template <typename KEY_OPERATION>
    static wp<TSD<K, REF<TS_T>>> select_keys(auto lhs, auto rhs, KEY_OPERATION key_operation) {
        const auto keys = key_operation(operators::keys_(lhs), operators::keys_(rhs));
        const auto raw_lhs = static_cast<WiringPoint>(lhs).template as<TSD<K, TS_T, G>>();
        return raw_lhs[keys].template as<TSD<K, REF<TS_T>>>();
    }

    template <bool SYMMETRIC> struct reference_routing_node {
        In<TSD<K, REF<TS_T>, G>> lhs;
        In<TSD<K, REF<TS_T>, G>> rhs;
        In<TSS<K, G>> lhs_keys;
        In<TSS<K, G>> rhs_keys;
        Out<TSD<K, REF<TS_T, TimeSeriesGuarantee::ordinary>>> output;

        void evaluate() const {
            if (!output.valid() || lhs.became_valid() || rhs.became_valid() || lhs_keys.became_valid() ||
                rhs_keys.became_valid()) {
                reconcile();
                return;
            }

            if (lhs_keys.modified()) {
                lhs_keys.for_each_added([&](const auto &key) { update(key); });
                lhs_keys.for_each_removed([&](const auto &key) { update(key); });
            }
            if (rhs_keys.modified()) {
                rhs_keys.for_each_added([&](const auto &key) { update(key); });
                rhs_keys.for_each_removed([&](const auto &key) { update(key); });
            }
            if (lhs.modified()) {
                lhs.for_each_modified([&](const auto &key) {
                    if (lhs_keys.contains(key)) {
                        update(key);
                    }
                });
            }
            if (rhs.modified()) {
                rhs.for_each_modified([&](const auto &key) {
                    if (!lhs_keys.contains(key) && rhs_keys.contains(key)) {
                        update(key);
                    }
                });
            }
        }

        bool selected(const auto &key) const {
            const auto in_lhs = lhs_keys.contains(key);
            const auto in_rhs = rhs_keys.contains(key);
            if constexpr (SYMMETRIC) {
                return in_lhs != in_rhs;
            }
            return in_lhs || in_rhs;
        }

        void update(const auto &key) const {
            if (!selected(key)) {
                output.erase(key);
                return;
            }

            if (lhs_keys.contains(key)) {
                output[key].copy(lhs[key]);
            } else {
                output[key].copy(rhs[key]);
            }
        }

        void reconcile() const {
            if (!output.valid()) {
                output.clear();
            }
            output.for_each_key([&](const auto &key) {
                if (!selected(key)) {
                    output.erase(key);
                }
            });
            lhs_keys.for_each([&](const auto &key) { update(key); });
            rhs_keys.for_each([&](const auto &key) { update(key); });
        }
    };

    template <bool SYMMETRIC>
    static wp<TSD<K, REF<TS_T, TimeSeriesGuarantee::ordinary>>> route(auto lhs, auto rhs) {
        const auto lhs_keys = operators::keys_(lhs);
        const auto rhs_keys = operators::keys_(rhs);
        const auto lhs_references = static_cast<WiringPoint>(lhs).template as<TSD<K, REF<TS_T>, G>>();
        const auto rhs_references = static_cast<WiringPoint>(rhs).template as<TSD<K, REF<TS_T>, G>>();
        static const auto route_node = bg::node<reference_routing_node<SYMMETRIC>>();
        return route_node(lhs_references, rhs_references, lhs_keys, rhs_keys);
    }

    overload<bit_and> bit_and_tsd{graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>> rhs) {
        return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
            return operators::bit_and(lhs_keys, rhs_keys);
        });
    })};
    overload<bit_and> bit_and_tsd_left_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::bit_and(lhs_keys, rhs_keys);
            });
        })};
    overload<bit_and> bit_and_tsd_right_valid_items{
        graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::bit_and(lhs_keys, rhs_keys);
            });
        })};
    overload<bit_and> bit_and_tsd_both_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::bit_and(lhs_keys, rhs_keys);
            });
        })};

    overload<sub> sub_tsd{graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>> rhs) {
        return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
            return operators::sub(lhs_keys, rhs_keys);
        });
    })};
    overload<sub> sub_tsd_left_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::sub(lhs_keys, rhs_keys);
            });
        })};
    overload<sub> sub_tsd_right_valid_items{
        graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::sub(lhs_keys, rhs_keys);
            });
        })};
    overload<sub> sub_tsd_both_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return select_keys(lhs, rhs, [](auto lhs_keys, auto rhs_keys) {
                return operators::sub(lhs_keys, rhs_keys);
            });
        })};

    overload<bit_or> bit_or_tsd{graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>> rhs) {
        return route<false>(lhs, rhs);
    })};
    overload<bit_or> bit_or_tsd_left_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>> rhs) {
            return route<false>(lhs, rhs);
        })};
    overload<bit_or> bit_or_tsd_right_valid_items{
        graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return route<false>(lhs, rhs);
        })};
    overload<bit_or> bit_or_tsd_both_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return route<false>(lhs, rhs);
        })};

    overload<bit_xor> bit_xor_tsd{graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>> rhs) {
        return route<true>(lhs, rhs);
    })};
    overload<bit_xor> bit_xor_tsd_left_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>> rhs) {
            return route<true>(lhs, rhs);
        })};
    overload<bit_xor> bit_xor_tsd_right_valid_items{
        graph([](wp<TSD<K, TS_T, G>> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return route<true>(lhs, rhs);
        })};
    overload<bit_xor> bit_xor_tsd_both_valid_items{
        graph([](wp<TSD<K, TS_T, G>, valid_items> lhs, wp<TSD<K, TS_T, G>, valid_items> rhs) {
            return route<true>(lhs, rhs);
        })};
};

} // namespace

void register_tss_operators() {
    register_overloads<tss_overloads>();
    register_overloads<tsd_set_overloads>();
}

} // namespace bg::operators::details

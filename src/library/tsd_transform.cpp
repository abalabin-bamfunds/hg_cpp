#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>

namespace bg::operators::details {
namespace {

using KEY = ScalarVariable<"REKEY_KEY", Hashable>;
using NEW_KEY = ScalarVariable<"REKEY_NEW_KEY", EqualityComparable, Hashable>;
using PARTITION = ScalarVariable<"PARTITION", EqualityComparable, Hashable>;
using INNER_KEY = ScalarVariable<"INNER_KEY", Hashable>;
using FLIP_VALUE = ScalarVariable<"FLIP_VALUE", EqualityComparable, Hashable>;

struct tsd_transform_overloads {
    struct rekey_tsd_impl {
        In<TSD<KEY, REF<TS_T>>, optional> ts;
        In<TSD<KEY, TS<NEW_KEY>>, optional, valid_items> new_keys;
        State<Dict<KEY, NEW_KEY>> previous_destination;
        Out<TSD<NEW_KEY, REF<TS_T>>> output;

        void evaluate() const {
            if (!ts.modified() && !new_keys.modified()) {
                return;
            }

            if (ts.modified()) {
                ts.for_each_removed([&](const auto &key) { remove_previous_destination(key); });
            }
            if (new_keys.modified()) {
                new_keys.for_each_removed([&](const auto &key) { remove_previous_destination(key); });
                new_keys.for_each_updated([&](const auto &key) { remove_changed_destination(key); });
            }

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            const auto assign_source_change = [&](const auto &key) {
                if (new_keys.contains(key)) {
                    assign_source(key, ts[key], new_keys[key].value_view());
                }
            };
            const auto assign_destination_change = [&](const auto &key) {
                if (ts.contains(key)) {
                    assign_source(key, ts[key], new_keys[key].value_view());
                }
            };

            if (ts.modified()) {
                ts.for_each_modified(assign_source_change);
            }
            if (new_keys.modified()) {
                new_keys.for_each_modified(assign_destination_change);
            }
        }

      private:
        void remove_changed_destination(const Scalar<KEY> &key) const {
            if (!previous_destination.contains(key)) {
                return;
            }

            if (previous_destination.at(key) != new_keys[key].value_view()) {
                remove_previous_destination(key);
            }
        }

        void assign_source(const Scalar<KEY> &key, const auto &source, const Scalar<NEW_KEY> &destination) const {
            output[destination].copy(source);
            previous_destination.insert_or_assign(key, destination);
        }

        void remove_previous_destination(const Scalar<KEY> &key) const {
            if (!previous_destination.contains(key)) {
                return;
            }

            output.erase(previous_destination.at(key));
            (void)previous_destination.erase(key);
        }
    };

    overload<rekey, rekey_tsd_impl> rekey_tsd;

    struct partition_tsd_impl {
        In<TSD<KEY, REF<TS_T>>, optional> ts;
        In<TSD<KEY, TS<PARTITION>>, optional, valid_items> partitions;
        Scalar<bool> remove_empty;
        State<Dict<KEY, PARTITION>> previous_partition;
        Out<TSD<PARTITION, TSD<KEY, REF<TS_T>>>> output;

        void evaluate() const {
            if (!ts.modified() && !partitions.modified()) {
                return;
            }

            if (ts.modified()) {
                ts.for_each_removed([&](const auto &key) { remove_previous_partition(key); });
            }
            if (partitions.modified()) {
                partitions.for_each_removed([&](const auto &key) { remove_previous_partition(key); });
                partitions.for_each_updated([&](const auto &key) { remove_changed_partition(key); });
            }

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            const auto assign_source_change = [&](const auto &key) {
                if (partitions.contains(key)) {
                    assign_source(key, ts[key], partitions[key].value_view());
                }
            };
            const auto assign_partition_change = [&](const auto &key) {
                if (ts.contains(key)) {
                    assign_source(key, ts[key], partitions[key].value_view());
                }
            };

            if (ts.modified()) {
                ts.for_each_modified(assign_source_change);
            }
            if (partitions.modified()) {
                partitions.for_each_modified(assign_partition_change);
            }
        }

      private:
        void remove_changed_partition(const Scalar<KEY> &key) const {
            if (!previous_partition.contains(key)) {
                return;
            }

            if (previous_partition.at(key) != partitions[key].value_view()) {
                remove_previous_partition(key);
            }
        }

        void assign_source(const Scalar<KEY> &key, const auto &source, const Scalar<PARTITION> &partition) const {
            output[partition][key].copy(source);
            previous_partition.insert_or_assign(key, partition);
        }

        void remove_previous_partition(const Scalar<KEY> &key) const {
            if (!previous_partition.contains(key)) {
                return;
            }

            const auto partition = previous_partition.at(key);
            if (output.contains(partition)) {
                const auto group = output[partition];
                group.erase(key);
                if (remove_empty.value() && group.empty()) {
                    output.erase(partition);
                }
            }
            (void)previous_partition.erase(key);
        }
    };

    overload<partition, partition_tsd_impl> partition_tsd;

    struct unpartition_tsd_impl {
        In<TSD<PARTITION, TSD<KEY, REF<TS_T>>>, optional> ts;
        Out<TSD<KEY, REF<TS_T>>> output;

        void evaluate() const {
            if (!ts.modified()) {
                return;
            }

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            for (auto id = ts.next_removed_id(0); id != util::occupancy_index::npos;
                 id = ts.next_removed_id(id + 1)) {
                remove_group(ts.item_at(id));
            }

            ts.for_each_added([&](const auto &partition) { add_group(ts[partition]); });

            ts.for_each_updated([&](const auto &partition) {
                const auto group = ts[partition];
                for (auto id = group.next_removed_id(0); id != util::occupancy_index::npos;
                     id = group.next_removed_id(id + 1)) {
                    output.erase(group.key_at(id));
                }
            });

            ts.for_each_updated([&](const auto &partition) {
                const auto group = ts[partition];
                group.for_each_modified([&](const auto &key) { output[key].copy(group[key]); });
            });
        }

      private:
        void add_group(const auto &group) const {
            group.for_each_key([&](const auto &key) { output[key].copy(group[key]); });
        }

        void remove_group(const auto &group) const {
            group.for_each_key([&](const auto &key) { output.erase(key); });
            group.for_each_removed([&](const auto &key) { output.erase(key); });
        }
    };

    overload<unpartition, unpartition_tsd_impl> unpartition_tsd;

    struct flip_unique_impl {
        In<TSD<KEY, TS<FLIP_VALUE>>, optional, valid_items> ts;
        Scalar<bool> unique;
        State<Dict<KEY, FLIP_VALUE>> previous_value;
        Out<TSD<FLIP_VALUE, TS<KEY>>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return scalar_cast<bool>(*arguments.scalar("unique")).value()
                       ? accept
                       : reject("flip with unique=false produces a TSS-valued dictionary");
        }

        void evaluate() const {
            ts.for_each_removed([&](const auto &key) { remove_source(key); });
            ts.for_each_updated([&](const auto &key) { remove_changed_source(key); });

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            ts.for_each_modified([&](const auto &key) { assign_source(key); });
        }

      private:
        void remove_changed_source(const Scalar<KEY> &key) const {
            if (!previous_value.contains(key) || previous_value.at(key) == ts[key].value_view()) {
                return;
            }
            remove_source(key);
        }

        void assign_source(const Scalar<KEY> &key) const {
            const auto value = ts[key].value_view();
            output[value].set_value(key);
            previous_value.insert_or_assign(key, value);
        }

        void remove_source(const Scalar<KEY> &key) const {
            if (!previous_value.contains(key)) {
                return;
            }

            const auto value = previous_value.at(key);
            output.erase(value);
            (void)previous_value.erase(key);
        }
    };

    overload<flip, flip_unique_impl> flip_unique;

    struct flip_grouped_impl {
        In<TSD<KEY, TS<FLIP_VALUE>>, optional, valid_items> ts;
        Scalar<bool> unique;
        State<Dict<KEY, FLIP_VALUE>> previous_value;
        Out<TSD<FLIP_VALUE, TSS<KEY>>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return !scalar_cast<bool>(*arguments.scalar("unique")).value()
                       ? accept
                       : reject("flip with unique=true produces a scalar-valued dictionary");
        }

        void evaluate() const {
            ts.for_each_removed([&](const auto &key) { remove_source(key); });
            ts.for_each_updated([&](const auto &key) { remove_changed_source(key); });

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            ts.for_each_modified([&](const auto &key) { assign_source(key); });
        }

      private:
        void remove_changed_source(const Scalar<KEY> &key) const {
            if (!previous_value.contains(key) || previous_value.at(key) == ts[key].value_view()) {
                return;
            }
            remove_source(key);
        }

        void assign_source(const Scalar<KEY> &key) const {
            const auto value = ts[key].value_view();
            output[value].insert(key);
            previous_value.insert_or_assign(key, value);
        }

        void remove_source(const Scalar<KEY> &key) const {
            if (!previous_value.contains(key)) {
                return;
            }

            const auto value = previous_value.at(key);
            if (output.contains(value)) {
                const auto keys = output[value];
                keys.erase(key);
                if (keys.empty()) {
                    output.erase(value);
                }
            }
            (void)previous_value.erase(key);
        }
    };

    overload<flip, flip_grouped_impl> flip_grouped;

    struct flip_keys_tsd_impl {
        In<TSD<KEY, TSD<INNER_KEY, REF<TS_T>>>, optional> ts;
        Scalar<bool> remove_empty;
        Out<TSD<INNER_KEY, TSD<KEY, REF<TS_T>>>> output;

        void evaluate() const {
            if (!ts.modified()) {
                return;
            }

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            for (auto id = ts.next_removed_id(0); id != util::occupancy_index::npos;
                 id = ts.next_removed_id(id + 1)) {
                remove_group(ts.key_at(id), ts.item_at(id));
            }

            ts.for_each_added([&](const auto &key) { add_group(key, ts[key]); });

            ts.for_each_updated([&](const auto &key) {
                const auto group = ts[key];
                for (auto id = group.next_removed_id(0); id != util::occupancy_index::npos;
                     id = group.next_removed_id(id + 1)) {
                    remove_item(key, group.key_at(id));
                }
            });

            ts.for_each_updated([&](const auto &key) {
                const auto group = ts[key];
                group.for_each_modified([&](const auto &inner_key) { assign_item(key, inner_key, group); });
            });
        }

      private:
        void add_group(const auto &key, const auto &group) const {
            group.for_each_key([&](const auto &inner_key) { assign_item(key, inner_key, group); });
        }

        void remove_group(const auto &key, const auto &group) const {
            group.for_each_key([&](const auto &inner_key) { remove_item(key, inner_key); });
            group.for_each_removed([&](const auto &inner_key) { remove_item(key, inner_key); });
        }

        void assign_item(const auto &key, const auto &inner_key, const auto &group) const {
            output[inner_key][key].copy(group[inner_key]);
        }

        void remove_item(const auto &key, const auto &inner_key) const {
            if (!output.contains(inner_key)) {
                return;
            }

            const auto group = output[inner_key];
            group.erase(key);
            if (remove_empty.value() && group.empty()) {
                output.erase(inner_key);
            }
        }
    };

    overload<flip_keys, flip_keys_tsd_impl> flip_keys_tsd;

    struct collapse_keys_tsd_impl {
        In<TSD<KEY, TSD<INNER_KEY, REF<TS_T>>>, optional> ts;
        Out<TSD<Tuple<KEY, INNER_KEY>, REF<TS_T>>> output;

        void evaluate() const {
            if (!ts.modified()) {
                return;
            }

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            for (auto id = ts.next_removed_id(0); id != util::occupancy_index::npos;
                 id = ts.next_removed_id(id + 1)) {
                remove_group(ts.key_at(id), ts.item_at(id));
            }

            ts.for_each_added([&](const auto &key) { add_group(key, ts[key]); });

            ts.for_each_updated([&](const auto &key) {
                const auto group = ts[key];
                for (auto id = group.next_removed_id(0); id != util::occupancy_index::npos;
                     id = group.next_removed_id(id + 1)) {
                    remove_item(key, group.key_at(id));
                }
            });

            ts.for_each_updated([&](const auto &key) {
                const auto group = ts[key];
                group.for_each_modified([&](const auto &inner_key) { assign_item(key, inner_key, group); });
            });
        }

      private:
        void add_group(const auto &key, const auto &group) const {
            group.for_each_key([&](const auto &inner_key) { assign_item(key, inner_key, group); });
        }

        void remove_group(const auto &key, const auto &group) const {
            group.for_each_key([&](const auto &inner_key) { remove_item(key, inner_key); });
            group.for_each_removed([&](const auto &inner_key) { remove_item(key, inner_key); });
        }

        void assign_item(const auto &key, const auto &inner_key, const auto &group) const {
            output[{key, inner_key}].copy(group[inner_key]);
        }

        void remove_item(const auto &key, const auto &inner_key) const {
            output.erase({key, inner_key});
        }
    };

    overload<collapse_keys, collapse_keys_tsd_impl> collapse_keys_tsd;

    struct uncollapse_keys_tsd_impl {
        In<TSD<Tuple<KEY, INNER_KEY>, REF<TS_T>>, optional> ts;
        Scalar<bool> remove_empty;
        Out<TSD<KEY, TSD<INNER_KEY, REF<TS_T>>>> output;

        void evaluate() const {
            ts.for_each_removed([&](const auto &key) {
                remove_item(key.template get<0>(), key.template get<1>());
            });

            if (ts.valid() && !output.valid()) {
                output.clear();
            }

            ts.for_each_modified([&](const auto &key) { assign_item(key); });
        }

      private:
        void assign_item(const Scalar<Tuple<KEY, INNER_KEY>> &key) const {
            const auto outer_key = key.template get<0>();
            const auto inner_key = key.template get<1>();
            output[outer_key][inner_key].copy(ts[key]);
        }

        void remove_item(const Scalar<KEY> &outer_key, const Scalar<INNER_KEY> &inner_key) const {
            if (!output.contains(outer_key)) {
                return;
            }

            const auto group = output[outer_key];
            group.erase(inner_key);
            if (remove_empty.value() && group.empty()) {
                output.erase(outer_key);
            }
        }
    };

    overload<uncollapse_keys, uncollapse_keys_tsd_impl> uncollapse_keys_tsd;

};

} // namespace

void register_tsd_transform_operators() { register_overloads<tsd_transform_overloads>(); }

} // namespace bg::operators::details

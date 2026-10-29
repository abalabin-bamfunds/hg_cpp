#include "registration.hpp"
#include "utf8.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <cstddef>
#include <string_view>
#include <type_traits>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;
using K = ScalarVariable<"K", Hashable>;
using VALUE = ScalarVariable<"COLLECTION_VALUE">;
using HASHABLE_VALUE = ScalarVariable<"COLLECTION_HASHABLE_VALUE", EqualityComparable, Hashable>;

struct collection_overloads {
    // Length.
    overload<len_> len_tss{
        [](In<TSS<SCALAR, G>> value, Out<TS<i64, G>> output) { output.set(static_cast<i64>(value.size())); }};

    overload<len_> len_tsl{graph(
        require = [](const WiringArguments &arguments) {
            const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type(0));
            return type_cast<TimeSeriesListType>(type.target_type()).item_count() != std::dynamic_extent;
        }, [](wp<TSL<TS_T, SIZE>> value) {
            const auto &type = type_cast<TimeSeriesListType>(value.type().target_type());
            return operators::const_(static_cast<i64>(type.item_count())).as<TS<i64, TimeSeriesGuarantee::const_>>();
        })};

    overload<len_> len_tsb{graph([](wp<TSB<SCHEMA>> value) {
        const auto &type = type_cast<TimeSeriesBundleType>(value.type().target_type());
        return operators::const_(static_cast<i64>(type.item_count())).as<TS<i64, TimeSeriesGuarantee::const_>>();
    })};

    overload<len_> len_list{
        [](In<TS<List<SCALAR>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> len_set{
        [](In<TS<Set<K>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> len_dict{
        [](In<TS<Dict<K, SCALAR>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> len_string{[](std::string_view value) { return static_cast<i64>(Utf8View{value}.length()); }};
    overload<len_> len_symbol{[](symbol value) { return static_cast<i64>(Utf8View{value}.length()); }};

    // Collection values.
    static RequirementResult require_values_list_output(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        const auto *resolved = arguments.resolved_output_type();
        return requested == nullptr || resolved == nullptr || requested->accepts(*resolved)
                   ? accept
                   : reject("values_ requires an output type compatible with the collection values");
    }

    static RequirementResult require_values_set_output(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        const auto *resolved = arguments.resolved_output_type();
        if (requested == nullptr || resolved == nullptr || !requested->accepts(*resolved)) {
            return reject("TSD values_ requires an explicitly selected compatible TSS result");
        }
        return accept;
    }

    overload<values_> values_scalar_dict{node(
        eval = [](In<TS<Dict<K, VALUE>, G>> dictionary, Out<TS<List<VALUE>, G>> output) {
            auto values = std::vector<Scalar<VALUE>>{};
            const auto dictionary_values = dictionary.value_view();
            values.reserve(dictionary_values.size());
            dictionary_values.for_each([&](const auto &, const auto &value) { values.push_back(value); });
            output.set(values);
        },
        require = require_values_list_output)};

    static const TimeSeriesType *requested_output(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<OUT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested == nullptr || *requested == nullptr ? nullptr : type_cast<TimeSeriesType>(*requested);
    }

    overload<values_> values_tsd_list{graph(
        require = require_values_list_output, [](wp<TSD<K, TS<VALUE, G>, G>> dictionary) {
            static const auto snapshot = node(
                eval = [](In<TSD<K, TS<VALUE, G>, G>, valid_items> dictionary,
                          Out<TS<List<VALUE>, G>> output) {
                    auto values = std::vector<Scalar<VALUE>>{};
                    values.reserve(dictionary.size());
                    dictionary.for_each_value([&](const auto &item) { values.push_back(item.value_view()); });
                    output.set(values);
                });
            return snapshot(valid_items(dictionary));
        })};

    struct values_tsd_set_impl {
        In<TSD<K, TS<HASHABLE_VALUE, G>, G>, valid_items> dictionary;
        State<Dict<K, HASHABLE_VALUE>> previous_values;
        State<Dict<HASHABLE_VALUE, i64>> value_counts;
        State<Set<HASHABLE_VALUE>> affected_values;
        Out<TSS<HASHABLE_VALUE, G>> output;

        void evaluate() const {
            if (!output.valid()) {
                rebuild();
                return;
            }

            dictionary.for_each_removed_key([&](const auto &key) { remove(key); });
            dictionary.for_each_modified_key_value([&](const auto &key, const auto &item) {
                update(key, item.value_view());
            });
            reconcile_affected_values();
        }

        void on_invalidation() const {
            previous_values->clear();
            value_counts->clear();
            affected_values->clear();
            output.invalidate();
        }

        void rebuild() const {
            output.clear();

            dictionary.for_each_key_value([&](const auto &key, const auto &item) {
                const auto value = item.value_view();
                previous_values->insert_or_assign(key, value);
                increment(value);
                output.insert(value);
            });
        }

        void update(const Scalar<K> &key, const Scalar<HASHABLE_VALUE> &value) const {
            const auto previous = previous_values->value().find(key);
            if (previous && previous->equals(value)) {
                return;
            }

            (void)affected_values->insert(value);
            if (previous) {
                (void)affected_values->insert(*previous);
                decrement(*previous);
            }

            increment(value);
            previous_values->insert_or_assign(key, value);
        }

        void remove(const Scalar<K> &key) const {
            const auto previous = previous_values->value().at(key);
            (void)affected_values->insert(previous);
            decrement(previous);
            (void)previous_values->erase(key);
        }

        void increment(const Scalar<HASHABLE_VALUE> &value) const {
            const auto previous = value_counts->value().find(value);
            const auto count = previous ? previous->value() : i64{};
            const auto next_count = count + 1;
            value_counts->insert_or_assign(value, Scalar<i64>::from(next_count));
        }

        void decrement(const Scalar<HASHABLE_VALUE> &value) const {
            const auto count = value_counts->value().at(value).value();
            if (count == 1) {
                (void)value_counts->erase(value);
                return;
            }

            const auto next_count = count - 1;
            value_counts->insert_or_assign(value, Scalar<i64>::from(next_count));
        }

        void reconcile_affected_values() const {
            const auto counts = value_counts->value();
            affected_values->value().for_each([&](const auto &value) {
                if (counts.contains(value)) {
                    output.insert(value);
                } else {
                    output.erase(value);
                }
            });
            affected_values->clear();
        }
    };

    overload<values_> values_tsd_set{graph(
        require = require_values_set_output, [](wp<TSD<K, TS<HASHABLE_VALUE, G>, G>> dictionary) {
            static const auto distinct_values = node<values_tsd_set_impl>();
            return distinct_values(valid_items(dictionary));
        })};

    // Collection item positions.
    overload<index_of> index_of_list_scalar{
        [](In<TS<List<EQUALITY_SCALAR>, G>> values, Scalar<EQUALITY_SCALAR> item, Out<TS<i64, G>> output) {
            const auto items = values.value_view();
            for (auto index = std::size_t{}; index < items.size(); ++index) {
                if (items[index].equals(item)) {
                    output.set(static_cast<i64>(index));
                    return;
                }
            }
            output.set(-1);
        }};

    overload<index_of> index_of_list_series{
        [](In<TS<List<EQUALITY_SCALAR>, G>> values, In<TS<EQUALITY_SCALAR, G>> item, Out<TS<i64, G>> output) {
            const auto items = values.value_view();
            for (auto index = std::size_t{}; index < items.size(); ++index) {
                if (items[index].equals(item.value_view())) {
                    output.set(static_cast<i64>(index));
                    return;
                }
            }
            output.set(-1);
        }};

    overload<index_of> index_of_tsl_scalar{node(
        [](In<TSL<TS<EQUALITY_SCALAR, G>, SIZE, G>> values, Scalar<EQUALITY_SCALAR> item, Out<TS<i64, G>> output) {
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                const auto value = values[index];
                if (value.valid() && value.value_view().equals(item)) {
                    output.set(static_cast<i64>(index));
                    return;
                }
            }
            output.set(-1);
        })};

    overload<index_of> index_of_tsl_series{node(
        [](In<TSL<TS<EQUALITY_SCALAR, G>, SIZE, G>> values, In<TS<EQUALITY_SCALAR, G>> item, Out<TS<i64, G>> output) {
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                const auto value = values[index];
                if (value.valid() && value.value_view().equals(item.value_view())) {
                    output.set(static_cast<i64>(index));
                    return;
                }
            }
            output.set(-1);
        })};

    // Membership.
    struct contains_tss_scalar_definition {
        static constexpr bool uses_cycle_callbacks = true;

        In<REF<TSS<K, G>, G>> collection;
        Scalar<K> key;
        In<TS<bool, G>, optional, dynamic_binding> membership;
        Out<TS<bool, G>> output;

        void evaluate() const {
            membership.bind(collection.membership(key));
            output.copy(membership);
        }

        void on_invalidation() const {
            membership.unbind();
            output.invalidate();
        }
    };

    overload<contains_> contains_tss_scalar{bg::node<contains_tss_scalar_definition>()};

    struct contains_tss_time_series_key_definition {
        static constexpr bool uses_cycle_callbacks = true;

        In<REF<TSS<K, G>, G>> collection;
        In<TS<K, G>> key;
        In<TS<bool, G>, optional, dynamic_binding> membership;
        Out<TS<bool, G>> output;

        void evaluate() const {
            membership.bind(collection.membership(key.value_view()));
            output.copy(membership);
        }

        void on_invalidation() const {
            membership.unbind();
            output.invalidate();
        }
    };

    overload<contains_> contains_tss_time_series_key{bg::node<contains_tss_time_series_key_definition>()};

    template <bool ValidItems> struct contains_tsd_scalar_definition {
        static constexpr bool uses_cycle_callbacks = true;

        In<REF<TSD<K, TS_T, G>, G>> collection;
        Scalar<K> key;
        In<TS<bool, G>, optional, dynamic_binding> membership;
        Out<TS<bool, G>> output;

        void evaluate() const {
            if constexpr (ValidItems) {
                membership.bind(collection.valid_item_membership(key));
            } else {
                membership.bind(collection.membership(key));
            }
            output.copy(membership);
        }

        void on_invalidation() const {
            membership.unbind();
            output.invalidate();
        }
    };

    overload<contains_> contains_tsd_scalar{bg::node<contains_tsd_scalar_definition<false>>()};

    overload<contains_> contains_tsd_physical_scalar{graph([](wp<TSD<K, TS_T, G>> collection, Scalar<K> key) {
        static const auto contains = bg::node<contains_tsd_scalar_definition<false>>();
        return contains(collection, key);
    })};

    overload<contains_> contains_tsd_valid_items_scalar{graph([](wp<TSD<K, TS_T, G>, valid_items> collection,
                                                                   Scalar<K> key) {
        static const auto contains = bg::node<contains_tsd_scalar_definition<true>>();
        return contains(static_cast<WiringPoint>(collection), key);
    })};

    template <bool ValidItems> struct contains_tsd_time_series_key_definition {
        static constexpr bool uses_cycle_callbacks = true;

        In<REF<TSD<K, TS_T, G>, G>> collection;
        In<TS<K, G>> key;
        In<TS<bool, G>, optional, dynamic_binding> membership;
        Out<TS<bool, G>> output;

        void evaluate() const {
            if constexpr (ValidItems) {
                membership.bind(collection.valid_item_membership(key.value_view()));
            } else {
                membership.bind(collection.membership(key.value_view()));
            }
            output.copy(membership);
        }

        void on_invalidation() const {
            membership.unbind();
            output.invalidate();
        }
    };

    overload<contains_> contains_tsd_time_series_key{bg::node<contains_tsd_time_series_key_definition<false>>()};

    overload<contains_> contains_tsd_physical_time_series_key{
        graph([](wp<TSD<K, TS_T, G>> collection, wp<TS<K, G>> key) {
            static const auto contains = bg::node<contains_tsd_time_series_key_definition<false>>();
            return contains(collection, key);
        })};

    overload<contains_> contains_tsd_valid_items_time_series_key{
        graph([](wp<TSD<K, TS_T, G>, valid_items> collection, wp<TS<K, G>> key) {
            static const auto contains = bg::node<contains_tsd_time_series_key_definition<true>>();
            return contains(static_cast<WiringPoint>(collection), key);
        })};

    overload<contains_> contains_list{
        [](In<TS<List<EQUALITY_SCALAR>, G>> collection, In<TS<EQUALITY_SCALAR, G>> item,
           Out<TS<bool, G>> output) {
            const auto values = collection.value_view();
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                if (values[index].equals(item.value_view())) {
                    output.set(true);
                    return;
                }
            }
            output.set(false);
        }};

    overload<contains_> contains_list_scalar{
        [](In<TS<List<EQUALITY_SCALAR>, G>> collection, Scalar<EQUALITY_SCALAR> item,
           Out<TS<bool, G>> output) {
            const auto values = collection.value_view();
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                if (values[index].equals(item)) {
                    output.set(true);
                    return;
                }
            }
            output.set(false);
        }};

    overload<contains_> contains_set{
        [](In<TS<Set<K>, G>> collection, In<TS<K, G>> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key.value_view()));
        }};

    overload<contains_> contains_set_scalar{
        [](In<TS<Set<K>, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key));
        }};

    overload<contains_> contains_dict{
        [](In<TS<Dict<K, SCALAR>, G>> collection, In<TS<K, G>> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key.value_view()));
        }};

    overload<contains_> contains_dict_scalar{
        [](In<TS<Dict<K, SCALAR>, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key));
        }};

    overload<contains_> contains_string{[](std::string_view value, std::string_view item) {
        return value.contains(item);
    }};
    overload<contains_> contains_symbol{[](symbol value, symbol item) {
        return value.text().contains(item.text());
    }};
};

} // namespace

void register_collection_operators() {
    register_overloads<collection_overloads>();
}

} // namespace bg::operators::details

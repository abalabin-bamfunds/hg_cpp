#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>

#include <cstddef>
#include <string_view>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"G">;
using K = ScalarVariable<"K", Hashable>;

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
    overload<len_> len_string{[](std::string_view value) { return static_cast<i64>(value.size()); }};
    overload<len_> len_symbol{[](symbol value) { return static_cast<i64>(value.text().size()); }};

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

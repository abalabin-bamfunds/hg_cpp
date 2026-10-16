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
    overload<contains_> contains_tss{
        [](In<TSS<K, G>> collection, In<TS<K, G>> key,
           Out<TS<bool, G>> output) {
            output.set(collection.contains(key.value_view()));
        }};

    overload<contains_> contains_tss_scalar{
        [](In<TSS<K, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.contains(key));
        }};

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

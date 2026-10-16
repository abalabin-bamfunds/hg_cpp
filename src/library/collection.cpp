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
    overload<len_> tss_length{
        [](In<TSS<SCALAR, G>> value, Out<TS<i64, G>> output) { output.set(static_cast<i64>(value.size())); }};

    overload<len_> fixed_list_length{graph(
        "name"_n = "len_fixed_list",
        require = [](const WiringArguments &arguments) {
            const auto &type = type_cast<TimeSeriesType>(arguments.resolved_type(0));
            return type_cast<TimeSeriesListType>(type.target_type()).item_count() != std::dynamic_extent;
        }, [](wp<TSL<TS_T, SIZE>> value) {
            const auto &type = type_cast<TimeSeriesListType>(value.type().target_type());
            return operators::const_(static_cast<i64>(type.item_count())).as<TS<i64, TimeSeriesGuarantee::const_>>();
        })};

    overload<len_> fixed_bundle_length{graph(
        "name"_n = "len_fixed_bundle", [](wp<TSB<SCHEMA>> value) {
            const auto &type = type_cast<TimeSeriesBundleType>(value.type().target_type());
            return operators::const_(static_cast<i64>(type.item_count())).as<TS<i64, TimeSeriesGuarantee::const_>>();
        })};

    overload<len_> list_length{
        [](In<TS<List<SCALAR>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> set_length{
        [](In<TS<Set<K>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> dictionary_length{
        [](In<TS<Dict<K, SCALAR>, G>> value, Out<TS<i64, G>> output) {
            output.set(static_cast<i64>(value.value_view().size()));
        }};
    overload<len_> string_length{[](std::string_view value) { return static_cast<i64>(value.size()); }};
    overload<len_> symbol_length{[](symbol value) { return static_cast<i64>(value.text().size()); }};

    // Membership.
    overload<contains_> set_membership{
        [](In<TSS<K, G>> collection, In<TS<K, G>> key,
           Out<TS<bool, G>> output) {
            output.set(collection.contains(key.value_view()));
        }};

    overload<contains_> set_membership_scalar{
        [](In<TSS<K, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.contains(key));
        }};

    overload<contains_> list_membership{
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

    overload<contains_> list_membership_scalar{
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

    overload<contains_> scalar_set_membership{
        [](In<TS<Set<K>, G>> collection, In<TS<K, G>> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key.value_view()));
        }};

    overload<contains_> scalar_set_membership_scalar{
        [](In<TS<Set<K>, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key));
        }};

    overload<contains_> scalar_dictionary_membership{
        [](In<TS<Dict<K, SCALAR>, G>> collection, In<TS<K, G>> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key.value_view()));
        }};

    overload<contains_> scalar_dictionary_membership_scalar{
        [](In<TS<Dict<K, SCALAR>, G>> collection, Scalar<K> key, Out<TS<bool, G>> output) {
            output.set(collection.value_view().contains(key));
        }};

    overload<contains_> string_substring{[](std::string_view value, std::string_view item) {
        return value.contains(item);
    }};
    overload<contains_> symbol_substring{[](symbol value, symbol item) {
        return value.text().contains(item.text());
    }};
};

} // namespace

void register_collection_operators() {
    (void)collection_overloads{};
}

} // namespace bg::operators::details

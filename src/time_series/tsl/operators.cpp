#include <bg/time_series/tsl/operators.hpp>

#include <bg/graph.hpp>
#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/time_series/size_variable.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_list_type.hpp>

#include <cstdint>

namespace bg::details {

namespace {

using V = TimeSeriesVariable<"V">;
inline constexpr auto G = guarantee_variable<"G">;

template <typename Item> struct tsl_getitem_time_series {
    In<REF<TSL<Item, SIZE, G>, G>, optional> tsl;
    In<TS<std::int64_t, G>, optional> index;
    In<REF<V, G>, dynamic_binding, optional> item;
    Out<REF<V, G>> output;

    void evaluate() const {
        if (!index.valid() || *index < 0) {
            item.unbind();
            output.invalidate();
            return;
        }

        item.bind(tsl[static_cast<std::size_t>(*index)]);
        output.copy(item);
    }
};

template <typename Item> struct tsl_getitem_scalar_reference {
    In<REF<TSL<Item, SIZE, G>, G>, optional> tsl;
    std::int64_t index;
    In<REF<V, G>, dynamic_binding, optional> item;
    Out<REF<V, G>> output;

    void evaluate() const {
        if (index < 0) {
            item.unbind();
            output.invalidate();
            return;
        }

        item.bind(tsl[static_cast<std::size_t>(index)]);
        output.copy(item);
    }
};

} // namespace

using namespace bg::literals;

void register_time_series_list_operators(Operator &getitem) {
    getitem.overload(bg::node<tsl_getitem_scalar_reference<V>>());
    getitem.overload(
        bg::graph("name"_n = "tsl_getitem", [](wp<TSL<TS_T, SIZE>> collection, std::int64_t index) -> wp<TS_T> {
            if (index < 0) {
                throw WiringError{"time series list index is out of range"};
            }

            return wp<TS_T>{time_series_list_item(collection, static_cast<std::size_t>(index))};
        }));
    getitem.overload(bg::node<tsl_getitem_time_series<V>>());
}

} // namespace bg::details

#include <bg/time_series/tsl/operators.hpp>

#include <bg/graph.hpp>
#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/time_series/size_variable.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_list_type.hpp>

#include <cstdint>

namespace bg::details {

using namespace bg::literals;

void register_time_series_list_operators(Operator &getitem) {
    getitem.overload(
        bg::graph("name"_n = "tsl_getitem", [](wp<TSL<TS_T, SIZE>> collection, std::int64_t index) -> wp<TS_T> {
            const auto &type = static_cast<const TimeSeriesListType &>(collection.type());
            if (index < 0 || static_cast<std::size_t>(index) >= type.item_count()) {
                throw WiringError{"time series list index is out of range"};
            }

            const auto position = static_cast<std::size_t>(index);
            if (const auto *free_list =
                    dynamic_cast<const FreeFixedTimeSeriesListWiringRecord *>(&wiring_record(collection))) {
                return wp<TS_T>{free_list->item(position)};
            }

            return wp<TS_T>{
                wiring_point_for_item(collection, type.item_id(position), type.item_type(position))};
        }));
}

} // namespace bg::details

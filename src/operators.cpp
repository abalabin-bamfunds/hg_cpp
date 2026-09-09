#include <bg/operators.hpp>

#include <bg/time_series/tsb/operators.hpp>
#include <bg/time_series/tsl/operators.hpp>

namespace bg::operators {

operator_<auto(COLLECTION, Union<K, TS<K>>) -> ITEM> getitem{"getitem", {"collection", "key"}};
operator_<auto(TSB<SCHEMA>, util::symbol) -> ITEM> getattr{"getattr", {"bundle", "name"}};

namespace {

const auto registered = [] {
    details::register_time_series_list_operators(getitem);
    details::register_time_series_bundle_operators(getattr);
    return true;
}();

} // namespace
} // namespace bg::operators

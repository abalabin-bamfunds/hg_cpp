#pragma once

namespace bg {

class TimeSeriesType;

namespace details {

template <typename Declaration> struct time_series_type_access {
    static decltype(auto) type();
};

} // namespace details

} // namespace bg

#pragma once

#include <utility>

namespace bg {

template <typename TimeSeries> class Out;

namespace details {

struct output_factory {
    template <typename TimeSeries, typename... Arguments>
    [[nodiscard]] static Out<TimeSeries> make(Arguments &&...arguments);
};

template <typename TimeSeries, typename... Arguments>
Out<TimeSeries> output_factory::make(Arguments &&...arguments) {
    return Out<TimeSeries>{std::forward<Arguments>(arguments)...};
}

} // namespace details

} // namespace bg

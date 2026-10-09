#pragma once

#include <bg/graph/wiring_point.hpp>
#include <bg/operators.hpp>

#include <utility>

namespace bg {

template <typename TimeSeries, auto... Properties>
template <typename Key>
auto wp<TimeSeries, Properties...>::operator[](Key &&key) const {
    return operators::getitem(*this, std::forward<Key>(key));
}

} // namespace bg

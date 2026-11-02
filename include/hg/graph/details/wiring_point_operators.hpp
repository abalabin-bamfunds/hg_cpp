#pragma once

#include <hg/graph/wiring_point.hpp>
#include <hg/operators.hpp>

#include <utility>

namespace hg {

template <typename TimeSeries, auto... Properties>
template <typename Key>
auto wp<TimeSeries, Properties...>::operator[](Key &&key) const {
    return operators::getitem(*this, std::forward<Key>(key));
}

} // namespace hg

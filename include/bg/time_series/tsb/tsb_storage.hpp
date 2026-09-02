#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/index.hpp>

#include <type_traits>

namespace bg::details {

struct time_series_bundle_storage {
    EvaluationTime change_time{MIN_DT};
    util::index<> modified;
};

static_assert(std::is_trivially_copyable_v<time_series_bundle_storage>);

} // namespace bg::details

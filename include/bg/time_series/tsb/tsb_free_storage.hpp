#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/index.hpp>

#include <type_traits>

namespace bg {

class CompiledNode;

namespace details {

struct free_time_series_bundle_storage {
    const CompiledNode *node{};
    EvaluationTime change_time{MIN_DT};
    util::index<> modified;
};

static_assert(std::is_trivially_copyable_v<free_time_series_bundle_storage>);

} // namespace details
} // namespace bg

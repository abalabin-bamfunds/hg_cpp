#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

#include <type_traits>

namespace bg {

class CompiledNode;

namespace details {

struct free_time_series_bundle_storage {
    static constexpr bool supports_byte_copy_initialization = true;

    const CompiledNode *node{};
    EvaluationTime change_time{MIN_DT};
    util::index<> modified;
};

static_assert(util::byte_copy_initializable<free_time_series_bundle_storage>);

} // namespace details
} // namespace bg

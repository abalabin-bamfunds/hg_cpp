#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

namespace bg {

class CompiledNode;

namespace details {

struct free_fixed_time_series_list_storage {
    static constexpr bool supports_byte_copy_initialization = true;

    const CompiledNode *node{};
    EvaluationTime change_time{MIN_DT};
    util::index<> updated;
};

static_assert(util::byte_copy_initializable<free_fixed_time_series_list_storage>);
static_assert(sizeof(free_fixed_time_series_list_storage) == 24);

} // namespace details
} // namespace bg

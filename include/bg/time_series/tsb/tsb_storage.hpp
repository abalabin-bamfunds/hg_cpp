#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

#include <type_traits>

namespace bg::details {

struct time_series_bundle_storage {
    static constexpr bool supports_byte_copy_initialization = true;

    EvaluationTime change_time{MIN_DT};
    util::index<> modified;
};

static_assert(util::byte_copy_initializable<time_series_bundle_storage>);

} // namespace bg::details

#pragma once

#include <bg/evaluation_time.hpp>
#include <bg/util/byte_copy.hpp>
#include <bg/util/index.hpp>

#include <cstdint>
#include <type_traits>

namespace bg::details {

struct time_series_bundle_storage {
    static constexpr bool supports_byte_copy_initialization = true;

    EvaluationTime change_time{MIN_DT};
    std::uint32_t valid_child_count{};
    util::index<> modified;
};

static_assert(util::byte_copy_initializable<time_series_bundle_storage>);
static_assert(sizeof(time_series_bundle_storage) == 24);

} // namespace bg::details

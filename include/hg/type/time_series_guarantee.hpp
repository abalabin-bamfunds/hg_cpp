#pragma once

namespace bg {

enum class TimeSeriesGuarantee {
    ordinary,
    deduplicated,
    final,
    const_,
};

constexpr bool satisfies(TimeSeriesGuarantee source, TimeSeriesGuarantee required) noexcept {
    return source >= required;
}

constexpr TimeSeriesGuarantee weakest(TimeSeriesGuarantee lhs, TimeSeriesGuarantee rhs) noexcept {
    return lhs < rhs ? lhs : rhs;
}

constexpr TimeSeriesGuarantee strongest(TimeSeriesGuarantee lhs, TimeSeriesGuarantee rhs) noexcept {
    return lhs > rhs ? lhs : rhs;
}

} // namespace bg

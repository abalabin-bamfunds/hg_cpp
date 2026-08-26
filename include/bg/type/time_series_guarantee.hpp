#pragma once

namespace bg {

enum class TimeSeriesGuarantee {
    ordinary,
    deduplicated,
    final,
    const_,
};

[[nodiscard]] constexpr bool satisfies(TimeSeriesGuarantee source,
                                       TimeSeriesGuarantee required) noexcept {
    return source >= required;
}

[[nodiscard]] constexpr TimeSeriesGuarantee weakest(TimeSeriesGuarantee lhs,
                                                     TimeSeriesGuarantee rhs) noexcept {
    return lhs < rhs ? lhs : rhs;
}

} // namespace bg

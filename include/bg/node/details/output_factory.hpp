#pragma once

#include <bg/node/output_declaration_traits.hpp>
#include <bg/time_series/output_binding.hpp>

#include <concepts>
#include <utility>

namespace bg {

namespace details {

struct output_factory {
    template <typename Output, typename... Arguments> [[nodiscard]] static Output make(Arguments &&...arguments);
};

template <typename Output, typename... Arguments> Output output_factory::make(Arguments &&...arguments) {
    if constexpr (std::constructible_from<Output, Arguments...>) {
        return Output{std::forward<Arguments>(arguments)...};
    } else {
        return Output{TimeSeriesOutputBinding{std::forward<Arguments>(arguments)...}};
    }
}

} // namespace details

} // namespace bg

#pragma once

#include <bg/graph/wiring_argument.hpp>

#include <span>
#include <utility>
#include <vector>

namespace bg {

class WiringTemplate;

struct default_switch_case_t {};
inline constexpr default_switch_case_t DEFAULT{};

struct SwitchCaseArgument {
    SwitchCaseArgument(WiringArgument key, const WiringTemplate &function)
        : keys{std::move(key)}, function{&function} {}

    SwitchCaseArgument(std::span<const WiringArgument> case_keys,
                       const WiringTemplate &function)
        : keys{case_keys.begin(), case_keys.end()}, function{&function} {}

    SwitchCaseArgument(default_switch_case_t, const WiringTemplate &function)
        : default_case{true}, function{&function} {}

    std::vector<WiringArgument> keys;
    bool default_case{};
    const WiringTemplate *function{};
};

} // namespace bg

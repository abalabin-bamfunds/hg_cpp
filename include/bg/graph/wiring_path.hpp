#pragma once

#include <bg/util/symbol.hpp>

#include <string>
#include <type_traits>
#include <vector>

namespace bg {

struct WiringPath {
    const WiringPath *parent;
    util::symbol name;
    util::symbol label;

    std::string text() const {
        auto sections = std::vector<const WiringPath *>{};
        for (auto *section = this; section != nullptr; section = section->parent) {
            sections.push_back(section);
        }

        auto result = std::string{};
        for (auto section = sections.rbegin(); section != sections.rend(); ++section) {
            if (!result.empty()) {
                result += '/';
            }
            result += (*section)->name.text();
            result += '[';
            result += (*section)->label.text();
            result += ']';
        }
        return result;
    }
};

static_assert(std::is_trivially_destructible_v<WiringPath>);

} // namespace bg

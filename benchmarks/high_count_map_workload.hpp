#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace high_count_map {

constexpr auto child_count = std::int64_t{8};
struct Profile {
    std::string_view name;
    std::int64_t flat_count;
    std::int64_t parent_count;
    std::size_t flat_updates;
    std::size_t child_updates;
    std::size_t flat_lifecycle_count;
    std::size_t parent_lifecycle_count;
    std::size_t child_lifecycle_count;

    std::uint64_t active_inner_graphs() const noexcept {
        return static_cast<std::uint64_t>(flat_count) * 2 +
               static_cast<std::uint64_t>(parent_count) * (child_count + 1);
    }
};

inline Profile profile(std::string_view name) {
    if (name == "smoke") {
        return {name, 8, 1, 7, 1, 1, 1, 1};
    }
    if (name == "25k") {
        return {name, 8'000, 1'000, 250, 250, 250, 25, 100};
    }
    if (name == "100k") {
        return {name, 32'000, 4'000, 250, 250, 250, 25, 100};
    }
    if (name == "250k") {
        return {name, 80'000, 10'000, 250, 250, 250, 25, 100};
    }

    throw std::invalid_argument{"unknown profile: " + std::string{name}};
}

constexpr std::uint64_t mix(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

enum class Branch : std::uint64_t { flat = 1, child = 2 };

struct EventChecksum {
    std::uint64_t count{};
    std::uint64_t sum{};
    std::uint64_t xor_value{};

    void add(std::size_t cycle, Branch branch, std::int64_t parent_key, std::int64_t item_key, bool present,
             std::int64_t value) noexcept {
        auto hash = mix(static_cast<std::uint64_t>(cycle));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(branch)));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(parent_key)));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(item_key)));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(present ? 1 : 0)));
        hash = mix(hash ^ mix(static_cast<std::uint64_t>(value)));

        ++count;
        sum += hash;
        xor_value ^= std::rotl(hash, static_cast<int>(hash & 63));
    }

    bool operator==(const EventChecksum &) const = default;
};

struct FlatMutation {
    std::int64_t key;
    std::int64_t value;

    bool operator==(const FlatMutation &) const = default;
};

struct ChildMutation {
    std::int64_t parent_key;
    std::int64_t child_key;
    std::int64_t value;

    bool operator==(const ChildMutation &) const = default;
};

enum class Lifecycle { none, remove_flat_and_parents, restore_flat_and_parents, remove_children, restore_children };

struct CyclePlan {
    std::vector<FlatMutation> flat_updates;
    std::vector<ChildMutation> child_updates;
    std::vector<std::int64_t> flat_lifecycle_keys;
    std::vector<std::int64_t> parent_lifecycle_keys;
    std::vector<std::pair<std::int64_t, std::int64_t>> child_lifecycle_keys;
    Lifecycle lifecycle{Lifecycle::none};
    std::optional<std::int64_t> bias;
};

struct Workload {
    Profile selected_profile;
    std::vector<CyclePlan> cycles;
    EventChecksum expected;
};

constexpr std::int64_t initial_flat_value(std::int64_t key) noexcept {
    return 1'000'004 + key * 17;
}

constexpr std::int64_t initial_child_value(std::int64_t parent_key, std::int64_t child_key) noexcept {
    return 1'000'036 + parent_key * 131 + child_key * 19;
}

constexpr std::int64_t flat_source_value(std::size_t cycle, std::int64_t key) noexcept {
    return static_cast<std::int64_t>(cycle + 2) * 1'000'003 + key * 17 + 1;
}

constexpr std::int64_t child_source_value(std::size_t cycle, std::int64_t parent_key,
                                          std::int64_t child_key) noexcept {
    return static_cast<std::int64_t>(cycle + 2) * 1'000'033 + parent_key * 131 + child_key * 19 + 3;
}

constexpr std::int64_t stage_a_value(std::int64_t value, std::int64_t key, std::int64_t bias) noexcept {
    const auto a1 = value + bias;
    const auto a2 = a1 * 3;
    const auto a3 = a2 + key;
    const auto a4 = a3 * 5;
    return a4 + 7;
}

constexpr std::int64_t stage_b_value(std::int64_t value, std::int64_t key, std::int64_t bias) noexcept {
    const auto b1 = value + bias;
    const auto b2 = b1 * 7;
    const auto b3 = b2 + key;
    const auto b4 = b3 * 3;
    return b4 + 11;
}

constexpr std::int64_t child_value(std::int64_t value, std::int64_t parent_key, std::int64_t child_key,
                                   std::int64_t bias) noexcept {
    const auto p1 = parent_key + bias;
    const auto seed = p1 * 5;
    const auto c1 = value + seed;
    const auto c2 = c1 * 3;
    const auto c3 = c2 + child_key;
    const auto c4 = c3 + bias;
    return c4 * 7;
}

inline std::size_t child_index(std::int64_t parent_key, std::int64_t child_key) noexcept {
    return static_cast<std::size_t>(parent_key) * child_count + child_key;
}

inline Workload build_workload(Profile selected_profile, std::size_t cycle_count) {
    auto result = Workload{.selected_profile = selected_profile, .cycles = {}, .expected = {}};
    result.cycles.reserve(cycle_count);

    auto flat_values = std::vector<std::int64_t>(selected_profile.flat_count);
    auto flat_present = std::vector<bool>(selected_profile.flat_count, true);
    for (auto key = std::int64_t{}; key < selected_profile.flat_count; ++key) {
        flat_values[key] = initial_flat_value(key);
    }

    const auto total_children = static_cast<std::size_t>(selected_profile.parent_count) * child_count;
    auto child_values = std::vector<std::int64_t>(total_children);
    auto parent_present = std::vector<bool>(selected_profile.parent_count, true);
    auto child_present = std::vector<bool>(total_children, true);
    for (auto parent_key = std::int64_t{}; parent_key < selected_profile.parent_count; ++parent_key) {
        for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
            child_values[child_index(parent_key, item_key)] = initial_child_value(parent_key, item_key);
        }
    }

    const auto flat_update_count =
        std::min(selected_profile.flat_updates, static_cast<std::size_t>(selected_profile.flat_count));
    const auto child_update_count =
        std::min(selected_profile.child_updates, static_cast<std::size_t>(selected_profile.parent_count));
    const auto flat_lifecycle_count =
        std::min(selected_profile.flat_lifecycle_count, static_cast<std::size_t>(selected_profile.flat_count));
    const auto parent_lifecycle_count =
        std::min(selected_profile.parent_lifecycle_count, static_cast<std::size_t>(selected_profile.parent_count));
    const auto child_lifecycle_count =
        std::min(selected_profile.child_lifecycle_count, static_cast<std::size_t>(selected_profile.parent_count));

    auto take_keys = [](std::int64_t size, std::size_t count, std::size_t start, auto &&excluded) {
        auto keys = std::vector<std::int64_t>{};
        keys.reserve(count);
        for (auto offset = std::int64_t{}; offset < size && keys.size() < count; ++offset) {
            const auto key = static_cast<std::int64_t>((start + static_cast<std::size_t>(offset)) % size);
            if (!excluded(key)) {
                keys.push_back(key);
            }
        }
        return keys;
    };

    auto bias = std::int64_t{1};

    for (auto cycle = std::size_t{}; cycle < cycle_count; ++cycle) {
        auto plan = CyclePlan{};
        plan.flat_updates.reserve(flat_update_count);
        plan.child_updates.reserve(child_update_count);
        const auto lifecycle_phase = cycle % 100;

        if (lifecycle_phase == 20) {
            plan.lifecycle = Lifecycle::remove_flat_and_parents;
            plan.flat_lifecycle_keys =
                take_keys(selected_profile.flat_count, flat_lifecycle_count, cycle * 251,
                          [&](std::int64_t key) { return !flat_present[static_cast<std::size_t>(key)]; });
            plan.parent_lifecycle_keys =
                take_keys(selected_profile.parent_count, parent_lifecycle_count, cycle * 29,
                          [&](std::int64_t key) { return !parent_present[static_cast<std::size_t>(key)]; });
        } else if (lifecycle_phase == 21) {
            plan.lifecycle = Lifecycle::restore_flat_and_parents;
            for (auto key = std::int64_t{}; key < selected_profile.flat_count; ++key) {
                if (!flat_present[static_cast<std::size_t>(key)]) {
                    plan.flat_lifecycle_keys.push_back(key);
                }
            }
            for (auto key = std::int64_t{}; key < selected_profile.parent_count; ++key) {
                if (!parent_present[static_cast<std::size_t>(key)]) {
                    plan.parent_lifecycle_keys.push_back(key);
                }
            }
        } else if (lifecycle_phase == 40) {
            plan.lifecycle = Lifecycle::remove_children;
            auto selected_parents = std::vector<bool>(selected_profile.parent_count);
            auto candidate = cycle * 101;
            while (plan.child_lifecycle_keys.size() < child_lifecycle_count) {
                const auto parent_key = static_cast<std::int64_t>(candidate % selected_profile.parent_count);
                const auto item_key = static_cast<std::int64_t>((cycle + parent_key) % child_count);
                const auto position = child_index(parent_key, item_key);
                if (!selected_parents[static_cast<std::size_t>(parent_key)] &&
                    parent_present[static_cast<std::size_t>(parent_key)] && child_present[position]) {
                    plan.child_lifecycle_keys.emplace_back(parent_key, item_key);
                    selected_parents[static_cast<std::size_t>(parent_key)] = true;
                }
                ++candidate;
            }
        } else if (lifecycle_phase == 41) {
            plan.lifecycle = Lifecycle::restore_children;
            for (auto parent_key = std::int64_t{}; parent_key < selected_profile.parent_count; ++parent_key) {
                for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                    if (!child_present[child_index(parent_key, item_key)]) {
                        plan.child_lifecycle_keys.emplace_back(parent_key, item_key);
                    }
                }
            }
        }

        const auto is_flat_lifecycle_key = [&](std::int64_t key) {
            return std::ranges::find(plan.flat_lifecycle_keys, key) != plan.flat_lifecycle_keys.end();
        };
        const auto flat_keys =
            take_keys(selected_profile.flat_count, flat_update_count, cycle * 257, [&](std::int64_t key) {
                return !flat_present[static_cast<std::size_t>(key)] || is_flat_lifecycle_key(key);
            });
        for (const auto key : flat_keys) {
            plan.flat_updates.push_back({key, flat_source_value(cycle, key)});
        }

        const auto is_parent_lifecycle_key = [&](std::int64_t key) {
            return std::ranges::find(plan.parent_lifecycle_keys, key) != plan.parent_lifecycle_keys.end();
        };
        const auto child_parents =
            take_keys(selected_profile.parent_count, child_update_count, cycle * 263, [&](std::int64_t key) {
                return !parent_present[static_cast<std::size_t>(key)] || is_parent_lifecycle_key(key);
            });
        for (const auto parent_key : child_parents) {
            auto item_key = static_cast<std::int64_t>((cycle + parent_key) % child_count);
            while (!child_present[child_index(parent_key, item_key)] ||
                   std::ranges::find(plan.child_lifecycle_keys, std::pair{parent_key, item_key}) !=
                       plan.child_lifecycle_keys.end()) {
                item_key = (item_key + 1) % child_count;
            }
            plan.child_updates.push_back({parent_key, item_key, child_source_value(cycle, parent_key, item_key)});
        }

        if (cycle % 1'000 == 50) {
            ++bias;
            plan.bias = bias;
        }

        for (const auto &[key, value] : plan.flat_updates) {
            flat_values[static_cast<std::size_t>(key)] = value;
        }
        for (const auto &[parent_key, item_key, value] : plan.child_updates) {
            child_values[child_index(parent_key, item_key)] = value;
        }

        if (plan.lifecycle == Lifecycle::remove_flat_and_parents) {
            for (const auto key : plan.flat_lifecycle_keys) {
                flat_present[static_cast<std::size_t>(key)] = false;
            }
            for (const auto parent_key : plan.parent_lifecycle_keys) {
                parent_present[static_cast<std::size_t>(parent_key)] = false;
            }
        } else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) {
            for (const auto key : plan.flat_lifecycle_keys) {
                flat_values[static_cast<std::size_t>(key)] = flat_source_value(cycle, key);
                flat_present[static_cast<std::size_t>(key)] = true;
            }
            for (const auto parent_key : plan.parent_lifecycle_keys) {
                parent_present[static_cast<std::size_t>(parent_key)] = true;
                for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                    const auto position = child_index(parent_key, item_key);
                    child_values[position] = child_source_value(cycle, parent_key, item_key);
                    child_present[position] = true;
                }
            }
        } else if (plan.lifecycle == Lifecycle::remove_children) {
            for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                child_present[child_index(parent_key, item_key)] = false;
            }
        } else if (plan.lifecycle == Lifecycle::restore_children) {
            for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                const auto position = child_index(parent_key, item_key);
                child_values[position] = child_source_value(cycle, parent_key, item_key);
                child_present[position] = true;
            }
        }

        if (plan.bias) {
            for (auto key = std::int64_t{}; key < selected_profile.flat_count; ++key) {
                if (flat_present[static_cast<std::size_t>(key)]) {
                    const auto value =
                        stage_b_value(stage_a_value(flat_values[static_cast<std::size_t>(key)], key, bias), key, bias);
                    result.expected.add(cycle, Branch::flat, 0, key, true, value);
                }
            }
            for (auto parent_key = std::int64_t{}; parent_key < selected_profile.parent_count; ++parent_key) {
                if (!parent_present[static_cast<std::size_t>(parent_key)]) {
                    continue;
                }
                for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                    const auto position = child_index(parent_key, item_key);
                    if (child_present[position]) {
                        const auto value = child_value(child_values[position], parent_key, item_key, bias);
                        result.expected.add(cycle, Branch::child, parent_key, item_key, true, value);
                    }
                }
            }
        } else {
            for (const auto &[key, value] : plan.flat_updates) {
                const auto output = stage_b_value(stage_a_value(value, key, bias), key, bias);
                result.expected.add(cycle, Branch::flat, 0, key, true, output);
            }
            for (const auto &[parent_key, item_key, value] : plan.child_updates) {
                const auto output = child_value(value, parent_key, item_key, bias);
                result.expected.add(cycle, Branch::child, parent_key, item_key, true, output);
            }

            if (plan.lifecycle == Lifecycle::remove_flat_and_parents) {
                for (const auto key : plan.flat_lifecycle_keys) {
                    result.expected.add(cycle, Branch::flat, 0, key, false, 0);
                }
                for (const auto parent_key : plan.parent_lifecycle_keys) {
                    for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                        const auto position = child_index(parent_key, item_key);
                        if (child_present[position]) {
                            result.expected.add(cycle, Branch::child, parent_key, item_key, false, 0);
                        }
                    }
                }
            } else if (plan.lifecycle == Lifecycle::restore_flat_and_parents) {
                for (const auto key : plan.flat_lifecycle_keys) {
                    const auto output =
                        stage_b_value(stage_a_value(flat_values[static_cast<std::size_t>(key)], key, bias), key, bias);
                    result.expected.add(cycle, Branch::flat, 0, key, true, output);
                }
                for (const auto parent_key : plan.parent_lifecycle_keys) {
                    for (auto item_key = std::int64_t{}; item_key < child_count; ++item_key) {
                        const auto position = child_index(parent_key, item_key);
                        const auto output = child_value(child_values[position], parent_key, item_key, bias);
                        result.expected.add(cycle, Branch::child, parent_key, item_key, true, output);
                    }
                }
            } else if (plan.lifecycle == Lifecycle::remove_children) {
                for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                    result.expected.add(cycle, Branch::child, parent_key, item_key, false, 0);
                }
            } else if (plan.lifecycle == Lifecycle::restore_children) {
                for (const auto &[parent_key, item_key] : plan.child_lifecycle_keys) {
                    const auto output =
                        child_value(child_values[child_index(parent_key, item_key)], parent_key, item_key, bias);
                    result.expected.add(cycle, Branch::child, parent_key, item_key, true, output);
                }
            }
        }

        result.cycles.push_back(std::move(plan));
    }

    return result;
}

inline void verify_workload_manifest() {
    static_assert(initial_flat_value(0) == 1'000'004);
    static_assert(initial_child_value(0, 0) == 1'000'036);
    static_assert(flat_source_value(0, 0) == 2'000'007);
    static_assert(child_source_value(0, 0, 0) == 2'000'069);

    const auto workload = build_workload(profile("smoke"), 51);
    auto require = [](bool condition, std::string_view description) {
        if (!condition) {
            throw std::logic_error{"workload manifest mismatch: " + std::string{description}};
        }
    };
    auto flat_keys = [](const CyclePlan &plan) {
        auto result = std::vector<std::int64_t>{};
        for (const auto &update : plan.flat_updates) {
            result.push_back(update.key);
        }
        return result;
    };

    require(flat_keys(workload.cycles[0]) == std::vector<std::int64_t>{0, 1, 2, 3, 4, 5, 6}, "cycle 0 flat keys");
    require(workload.cycles[0].child_updates == std::vector<ChildMutation>{{0, 0, 2'000'069}}, "cycle 0 child update");
    require(flat_keys(workload.cycles[20]) == std::vector<std::int64_t>{5, 6, 7, 0, 1, 2, 3}, "cycle 20 flat keys");
    require(workload.cycles[20].flat_lifecycle_keys == std::vector<std::int64_t>{4}, "cycle 20 flat removal");
    require(workload.cycles[20].parent_lifecycle_keys == std::vector<std::int64_t>{0}, "cycle 20 parent removal");
    require(workload.cycles[21].flat_lifecycle_keys == std::vector<std::int64_t>{4}, "cycle 21 flat restoration");
    require(workload.cycles[21].parent_lifecycle_keys == std::vector<std::int64_t>{0}, "cycle 21 parent restoration");
    require(workload.cycles[40].child_lifecycle_keys == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}},
            "cycle 40 child removal");
    require(workload.cycles[41].child_lifecycle_keys == std::vector<std::pair<std::int64_t, std::int64_t>>{{0, 0}},
            "cycle 41 child restoration");
    require(workload.cycles[50].bias == 2, "cycle 50 broadcast");
    require(workload.expected == EventChecksum{.count = 434,
                                               .sum = 10'494'966'632'268'150'872ULL,
                                               .xor_value = 5'460'338'408'372'608'614ULL},
            "51-cycle checksum");

    const auto production = build_workload(profile("25k"), 41);
    require(production.cycles[20].flat_lifecycle_keys.size() == 250 &&
                production.cycles[20].flat_lifecycle_keys.front() == 5'020 &&
                production.cycles[20].flat_lifecycle_keys.back() == 5'269,
            "production flat removals");
    require(production.cycles[20].parent_lifecycle_keys.size() == 25 &&
                production.cycles[20].parent_lifecycle_keys.front() == 580 &&
                production.cycles[20].parent_lifecycle_keys.back() == 604,
            "production parent removals");
    require(production.cycles[20].flat_updates.front().key == 5'270 &&
                production.cycles[20].flat_updates.back().key == 5'519,
            "production flat updates");
    require(production.cycles[40].child_lifecycle_keys.size() == 100 &&
                production.cycles[40].child_lifecycle_keys.front() == std::pair<std::int64_t, std::int64_t>{40, 0} &&
                production.cycles[40].child_lifecycle_keys.back() == std::pair<std::int64_t, std::int64_t>{139, 3},
            "production child removals");
}

} // namespace high_count_map

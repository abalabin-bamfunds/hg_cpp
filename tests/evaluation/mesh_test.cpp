#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/reduce.hpp>
#include <bg/switch.hpp>
#include <bg/run_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

const auto add_mesh_dependency = node([](i64 value, std::optional<i64> dependency) {
    return value + dependency.value_or(0);
});

TEST_CASE("mesh evaluates a dependency branch before the branch that depends on it",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[binding:reference-to-value][transition:addition]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 values, dependencies);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 10}, {1, 1}}, {{2, 30}, {1, 3}}}, {{{2, 1}}, {{2, 1}}});

    CHECK(actual.equals<bg::i64>({11, 33}));
}

const auto observe_mesh_dependency =
    node([](In<REF<TS<i64>, TimeSeriesGuarantee::deduplicated>, optional>) {});

TEST_CASE("mesh forwards optional positional input only to an optional branch parameter",
          "[feature:mesh][feature:nested-graph][feature:operator][feature:wiring][series:tsd]"
          "[validity:optional][definition:function]") {
    const auto dictionaries = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto values = node([](Out<TS<i64>>) {});

    const auto optional_branch = graph(
        "name"_n = "optional_mesh_branch",
        [](wp<TS<i64>> item, wp<TS<i64>, optional>) { return item; });

    const auto required_branch = graph(
        "name"_n = "required_mesh_branch",
        [](wp<TS<i64>> item, wp<TS<i64>>) { return item; });

    testing::test_wiring([&] {
        const auto dictionary = dictionaries();
        const auto context = values();

        CHECK_NOTHROW(mesh(optional_branch, dictionary, opt(context)));
        CHECK_THROWS_AS(mesh(required_branch, dictionary, opt(context)), TypeResolutionError);
    });
}

const auto mesh_values_at_key_five = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
    output[5].set(*value);
});

struct DelayedMeshValue {
    static constexpr bool uses_timed_scheduling = true;

    In<TS<i64>, passive> value;
    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const { output.set(*value); }
};

const auto delayed_mesh_value = node<DelayedMeshValue>();

TEST_CASE("mesh resumes a dependency node after its dependency completes",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition]") {
    auto events = std::vector<int>{};

    const auto record_mesh_dependency_key = node(
        [&events](In<TS<i64, TimeSeriesGuarantee::const_>> key,
                  In<TS<i64>> dependency_key, Out<TS<i64>> output) {
            events.push_back(static_cast<int>(*key * 10 + 1));
            output.set(*dependency_key);
        });

    const auto record_mesh_completion = node(
        [&events](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<i64>> value,
                  In<TS<i64>, optional> dependency, Out<TS<i64>> output) {
            events.push_back(static_cast<int>(*key * 10 + 2));
            output.set(*key + *value * 0 + (dependency.valid() ? *dependency : 0));
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [&](auto key, auto value, auto dependency_key) {
                const auto recorded_key = record_mesh_dependency_key(key, dependency_key);
                return record_mesh_completion(key, value, mesh()[recorded_key]);
            },
            values, dependencies, "__key_arg__"_n = 0);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 20}, {1, 10}}}, {{{2, 1}}});

    CHECK(actual.equals<i64>({3}));
    CHECK(events == std::vector{21, 12, 22});
}

TEST_CASE("mesh creates a branch for a requested missing key",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto key, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            dependencies, "__key_arg__"_n = 0);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 1}}});

    CHECK(actual.equals<i64>({1}));
}

TEST_CASE("mesh follows a time-series dependency key",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][series:value]"
          "[shape:nested][transition:update]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 values, dependencies);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}, {2, 10}, {3, 100}}, {{1, 2}, {2, 20}, {3, 200}}},
                                            {{{2, 1}}, {{2, 3}}});

    CHECK(actual.equals<bg::i64>({11, 220}));
}

TEST_CASE("mesh invalidates a dependency when the dependency branch output becomes invalid",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 values, dependencies);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}, {2, 10}}, {{1, literal::REMOVE}, {2, 20}}},
                                            {{{2, 1}}, {{2, 1}}});

    CHECK(actual.equals<bg::i64>({11, 20}));
}

TEST_CASE("mesh retains a dependency-only branch until its last dependent is removed",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            values, dependencies, "__key_arg__"_n = 0);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 7}}, {{2, literal::REMOVE}}, {{2, 9}}},
                                            {{{2, 1}}, {{2, literal::REMOVE}}, {{2, 1}}});

    CHECK(actual.equals<i64>({1, std::nullopt, 1}));
}

TEST_CASE("mesh destroys removed dependent branches and their dependency branches",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:removal][transition:lifecycle]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            values, dependencies, "__key_arg__"_n = 0);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 7}}, {{2, literal::REMOVE}}, {{2, literal::REMOVE}}},
                                            {{{2, 1}}, {{2, literal::REMOVE}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals<i64>({1, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh destroys a removed dependency while its dependent remains",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation][transition:lifecycle]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            values, dependencies, "__key_arg__"_n = 0);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 7}}, {{2, 8}}, {{2, 9}}},
                                            {{{2, 1}}, {{2, literal::REMOVE}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals<i64>({2, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh transfers a dependency after its previous dependent releases it",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation][transition:revalidation][transition:lifecycle]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            values, dependencies, "__key_arg__"_n = 0);
        return result[i64{3}];
    };

    const auto actual = bg::run_graph(tested,
                                            {{{2, 20}, {3, 30}}, {{2, 20}, {3, 30}}, {{2, 20}, {3, 30}}},
                                            {{{2, 1}}, {{2, literal::REMOVE}, {3, 1}}, {{3, 1}}});

    CHECK(actual.equals<i64>({3, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh rejects self and multi-branch dependency cycles",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 values, dependencies);
        return result[i64{1}];
    };

    CHECK_THROWS_AS(bg::run_graph(tested, {{{2, 1}}}, {{{1, 1}}}), std::runtime_error);
    CHECK_THROWS_AS(bg::run_graph(tested, {{{2, 1}}}, {{{1, 2}, {2, 1}}}), std::runtime_error);
}

TEST_CASE("mesh evaluates a shared dependency once",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[shape:diamond]") {
    auto evaluation_counts = std::array<int, 5>{};

    const auto count_mesh_branch = node(
        [&evaluation_counts](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<i64>> value,
                             In<TS<i64>, optional> first, In<TS<i64>, optional> second,
                             Out<TS<i64>> output) {
            ++evaluation_counts.at(static_cast<std::size_t>(*key));
            output.set(static_cast<int>(*key) + (first.valid() ? *first : 0) +
                       (second.valid() ? *second : 0));
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> first_dependencies,
                           wp<TSD<i64, TS<i64>>> second_dependencies) {
        const auto result = mesh(
            [&](auto key, auto value, auto first_key, auto second_key) {
                return count_mesh_branch(key, value, mesh()[first_key], mesh()[second_key]);
            },
            values, first_dependencies, second_dependencies,
            "__key_arg__"_n = 0);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 0}, {2, 0}, {3, 0}, {4, 0}}},
                                            {{{1, 2}, {2, 4}, {3, 4}}}, {{{1, 3}}});

    CHECK(actual.equals<bg::i64>({14}));
    CHECK(evaluation_counts[4] == 1);
}

TEST_CASE("mesh does not reevaluate a clean dependency branch",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:update]") {
    auto evaluation_counts = std::array<int, 3>{};

    const auto count_mesh_dependency = node(
        [&evaluation_counts](In<TS<i64, TimeSeriesGuarantee::const_>> key, In<TS<i64>> value,
                             In<TS<i64>, optional> dependency, Out<TS<i64>> output) {
            ++evaluation_counts.at(static_cast<std::size_t>(*key));
            output.set(*value + (dependency.valid() ? *dependency : 0));
        });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result =
            mesh([&](auto key, auto item,
                     auto dependency_key) { return count_mesh_dependency(key, item, mesh()[dependency_key]); },
                 values, dependencies, "__key_arg__"_n = 0);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}, {2, 10}}, {{2, 20}}}, {{{2, 1}}, {{2, 1}}});

    CHECK(actual.equals<bg::i64>({11, 21}));
    CHECK(evaluation_counts[1] == 1);
    CHECK(evaluation_counts[2] == 2);
}

TEST_CASE("mesh runs inside map", "[feature:mesh][feature:map][feature:nested-graph][feature:operator][series:tsd]"
                                  "[shape:doubly-nested]") {
    const auto mesh_values = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[2].set(*value * 10);
        output[1].set(*value);
    });

    const auto mesh_dependencies =
        node([](In<TS<i64>>, Out<TSD<i64, TS<i64>>> output) { output[2].set(1); });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto outer = map(
            [&](auto item) {
                const auto result = mesh(
                    [](auto inner, auto dependency_key) { return add_mesh_dependency(inner, mesh()[dependency_key]); },
                    mesh_values(item), mesh_dependencies(item));
                return result[i64{2}].template as<TS<i64>>();
            },
            values);
        return outer[i64{5}];
    };

    const auto actual = bg::run_graph(tested, {{{5, 1}}, {{5, 3}}});

    CHECK(actual.equals<bg::i64>({11, 33}));
}

TEST_CASE("map runs inside mesh", "[feature:mesh][feature:map][feature:nested-graph][feature:operator][series:tsd]"
                                  "[shape:doubly-nested]") {
    const auto increment_mesh_value = node([](i64 value) { return value + 1; });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto outer = mesh(
            [&](auto item) {
                const auto result =
                    map([&](auto inner) { return increment_mesh_value(inner); }, mesh_values_at_key_five(item));
                return result[i64{5}].template as<TS<i64>>();
            },
            values);
        return outer[i64{5}];
    };

    const auto actual = bg::run_graph(tested, {{{5, 1}}, {{5, 3}}});

    CHECK(actual.equals<bg::i64>({2, 4}));
}

TEST_CASE("a named outer mesh is selected through an inner mesh and switch",
          "[feature:mesh][feature:switch][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][transition:addition]") {
    const auto tested = [=](wp<TSD<i64, TS<i64>>> values) {
        const auto outer = mesh(
            "name"_n = "outer",
            [=](auto key, auto item) {
                return switch_(key,
                               cases{
                                   case_(i64{1}, [](auto leaf) { return leaf; }),
                                   case_(i64{2},
                                         [=](auto selected) {
                                             const auto inner = mesh(
                                                 "name"_n = "inner",
                                                 [](auto inner_value) {
                                                     return add_mesh_dependency(inner_value,
                                                                                mesh("outer")[i64{1}]);
                                                 },
                                                 mesh_values_at_key_five(selected));
                                             return inner[i64{5}].template as<TS<i64>>();
                                         }),
                               },
                               item)
                    .template as<TS<i64>>();
            },
            values, "__key_arg__"_n = 0);
        return outer[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 20}, {1, 2}}, {{2, 30}, {1, 3}}});

    CHECK(actual.equals<i64>({22, 33}));
}

TEST_CASE("a mesh dependency follows switch output replacement",
          "[feature:mesh][feature:switch][feature:nested-graph][feature:operator][feature:reference]"
          "[series:tsd][shape:deeply-nested][binding:reference-to-value][transition:update]"
          "[transition:lifecycle]") {
    const auto add_amount = node([](i64 value, int amount) { return value + amount; });

    const auto tested = [&](wp<TS<i64>> selected_case, wp<TSD<i64, TS<i64>>> values) {
        const auto result = mesh(
            [&](auto key, auto item, auto selected) {
                return switch_(
                           key,
                           cases{
                               case_(i64{1},
                                     [&](auto branch_value, auto branch_selected) {
                                         return switch_(
                                                    branch_selected,
                                                    cases{
                                                        case_(i64{1},
                                                              [&](auto selected_value) {
                                                                  return add_amount(selected_value, 100);
                                                              }),
                                                        case_(i64{2},
                                                              [&](auto selected_value) {
                                                                  return add_amount(selected_value, 200);
                                                              }),
                                                    },
                                                    branch_value)
                                             .template as<TS<i64>>();
                                     }),
                               case_(i64{2},
                                     [](auto branch_value, auto) {
                                         return add_mesh_dependency(branch_value, mesh()[i64{1}]);
                                     }),
                           },
                           item, selected)
                    .template as<TS<i64>>();
            },
            values, pass_through(selected_case), "__key_arg__"_n = 0);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {1, 2, 2, 1},
                                            {{{2, 10}, {1, 1}}, {{2, 20}, {1, 2}},
                                             {{2, 30}, {1, 3}}, {{2, 40}, {1, 4}}});

    CHECK(actual.equals<i64>({111, 222, 233, 144}));
}

TEST_CASE("runtime map instances keep independent outer mesh dependencies",
          "[feature:mesh][feature:map][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][shape:multiple]") {
    const auto inner_values = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        output[10].set(*value);
        output[20].set(*value * 2);
    });

    const auto inner_dependencies = node([](In<TS<i64>>, Out<TSD<i64, TS<i64>>> output) {
        output[10].set(1);
        output[20].set(2);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values) {
        const auto outer = mesh(
            "name"_n = "outer",
            [&](auto key, auto item) {
                return switch_(key,
                               cases{
                                   case_(std::tuple{i64{1}, i64{2}}, [](auto leaf) { return leaf; }),
                                   case_(i64{3},
                                         [&](auto selected) {
                                             const auto mapped = map(
                                                 [](auto inner, auto dependency) {
                                                     return add_mesh_dependency(inner, mesh("outer")[dependency]);
                                                 },
                                                 inner_values(selected), inner_dependencies(selected));
                                             return mapped[i64{20}].template as<TS<i64>>();
                                         }),
                               },
                               item)
                    .template as<TS<i64>>();
            },
            values, "__key_arg__"_n = 0);
        return outer[i64{3}];
    };

    const auto actual = bg::run_graph(tested, {{{1, 1}, {2, 10}, {3, 100}},
                                                     {{1, 2}, {2, 20}, {3, 200}}});

    CHECK(actual.equals<bg::i64>({210, 420}));
}

TEST_CASE("a map removes and recreates branches after pending mesh dependencies",
          "[feature:mesh][feature:map][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][transition:removal][transition:addition]") {
    const auto optional_inner_mesh_values = node([](In<TS<bool>> present, Out<TSD<i64, TS<i64>>> output) {
        if (*present) {
            output[1].set(0);
            output[2].set(0);
        } else {
            output.erase(1);
            output.erase(2);
        }
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> values, wp<TS<bool>> present) {
        const auto outer = mesh(
            "name"_n = "outer",
            [&](auto key, auto item, auto branch_present) {
                return switch_(key,
                               cases{
                                   case_(std::tuple{i64{1}, i64{2}},
                                         [](auto leaf, auto) { return leaf; }),
                                   case_(i64{3},
                                         [&](auto, auto selected_present) {
                                             const auto mapped = map(
                                                 [](auto selected_key, auto inner_value) {
                                                     return add_mesh_dependency(inner_value,
                                                                                mesh("outer")[selected_key]);
                                                 },
                                                 optional_inner_mesh_values(selected_present), "__key_arg__"_n = 0);
                                             return mapped[i64{2}].template as<TS<i64>>();
                                         }),
                               },
                               item, branch_present)
                    .template as<TS<i64>>();
            },
            values, pass_through(present), "__key_arg__"_n = 0);
        return outer[i64{3}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 1}, {2, 10}, {3, 100}}, {{1, 2}, {2, 20}, {3, 200}}, {{1, 3}, {2, 30}, {3, 300}}},
        {true, false, true});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("a mesh dependency branch reports timed work through its owner",
          "[feature:mesh][feature:nested-graph][feature:operator][feature:timed-scheduling]"
          "[series:tsd][shape:nested]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto item, auto dependency_key) {
                return add_mesh_dependency(delayed_mesh_value(item), mesh()[dependency_key]);
            },
            values, dependencies);
        return result[i64{2}];
    };

    const auto actual = bg::run_graph(tested, {{{2, 10}, {1, 1}}}, {{{2, 1}}});

    CHECK(actual.equals<i64>({std::nullopt, std::nullopt, 11}));
}

TEST_CASE("mesh supports source and sink graphs",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:source][shape:sink]") {
    auto recorded_values = std::vector<i64>{};

    const auto sink = node([&recorded_values](i64 value) { recorded_values.push_back(value); });

    const auto source_test = [](wp<TSS<i64>> keys) {
        return mesh([](auto key) { return key; }, keys)[i64{7}];
    };
    const auto source_result = bg::run_graph(source_test, {{7}, {literal::REMOVED(7)}, {7}});

    CHECK(source_result.equals<i64>({7, std::nullopt, 7}));

    const auto sink_test = [&](wp<TSD<i64, TS<i64>>> values) {
        mesh([&](auto item) { sink(item); }, values);
        return values[i64{5}];
    };
    const auto sink_result = bg::run_graph(sink_test, {{{5, 1}}, {{5, 2}}});

    CHECK(sink_result.equals<bg::i64>({1, 2}));
    CHECK(recorded_values == std::vector<i64>{1, 2});
}

const auto sum_mesh_values = node([](i64 left, i64 right) { return left + right; });

TEST_CASE("reduce combines mesh outputs as branches update and disappear",
          "[feature:mesh][feature:reduce][feature:nested-graph][series:tsd]"
          "[shape:doubly-nested][transition:update][transition:removal]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [](auto item, auto dependency_key) {
                return add_mesh_dependency(item, mesh()[dependency_key]);
            },
            values, dependencies);
        return reduce(sum_mesh_values, result, "default"_n = i64{0});
    };

    const auto actual = bg::run_graph(
        tested,
        {{{1, 1}, {2, 10}}, {{1, 3}, {2, 20}}, {{2, literal::REMOVE}, {1, 4}},
         {{1, literal::REMOVE}}},
        {{{2, 1}}, {{2, literal::REMOVE}}});

    CHECK(actual.equals<i64>({12, 23, 4, 0}));
}

TEST_CASE("mesh branches reduce nested dictionaries before using their dependencies",
          "[feature:mesh][feature:reduce][feature:nested-graph][series:tsd]"
          "[shape:deeply-nested][transition:update]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values,
                           wp<TSD<i64, TS<i64>>> dependency_keys) {
        const auto result = mesh(
            [](auto items, auto dependency_key) {
                const auto total = reduce(sum_mesh_values, items, "default"_n = i64{0});
                return add_mesh_dependency(total, mesh()[dependency_key]);
            },
            values, dependency_keys);
        return reduce(sum_mesh_values, result, "default"_n = i64{0});
    };

    const auto actual = bg::run_graph(
        tested,
        {{{1, {{10, 1}, {20, 2}}}, {2, {{10, 4}}}},
         {{1, {{10, 10}}}}},
        {{{2, 1}}});

    CHECK(actual.equals<i64>({10, 28}));
}

TEST_CASE("mapped meshes reduce independently and rebuild after a parent returns",
          "[feature:mesh][feature:map][feature:reduce][feature:nested-graph]"
          "[series:tsd][shape:deeply-nested][transition:update][transition:removal]"
          "[transition:addition]") {
    const auto tested = [](wp<TSD<i64, TSD<i64, TS<i64>>>> values,
                           wp<TSD<i64, TSD<i64, TS<i64>>>> dependency_keys) {
        const auto totals = map(
            [](auto inner_values, auto inner_dependencies) {
                const auto branches = mesh(
                    [](auto value, auto dependency_key) {
                        return add_mesh_dependency(value, mesh()[dependency_key]);
                    },
                    inner_values, inner_dependencies);
                return reduce(sum_mesh_values, branches, "default"_n = i64{0});
            },
            values, dependency_keys);
        return reduce(sum_mesh_values, totals, "default"_n = i64{0});
    };

    const auto actual = bg::run_graph(
        tested,
        {{{1, {{1, 1}, {2, 10}}}, {2, {{1, 3}, {2, 20}}}},
         {{1, {{1, 5}}}, {2, {{2, 30}}}},
         {{1, literal::REMOVE}},
         {{1, {{1, 7}, {2, 1}}}}},
        {{{1, {{2, 1}}}, {2, {{2, 1}}}}, {{1, {{2, 1}}}, {2, {{2, 1}}}},
         {{1, literal::REMOVE}}, {{1, {{2, 1}}}}});

    CHECK(actual.equals<i64>({38, 56, 36, 51}));
}

TEST_CASE("switch-selected mesh branches feed a reduction",
          "[feature:mesh][feature:switch][feature:reduce][feature:nested-graph]"
          "[series:tsd][shape:deeply-nested][transition:update]") {
    const auto add_amount = node([](i64 value, int amount) { return value + amount; });

    const auto tested = [=](wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [=](auto key, auto item, auto dependency_key) {
                const auto selected = add_mesh_dependency(item, mesh()[dependency_key]);
                return switch_(key,
                               cases{case_(i64{1}, [=](auto value) { return add_amount(value, 10); }),
                                     case_(i64{2}, [](auto value) { return value; })},
                               selected);
            },
            values, dependencies, "__key_arg__"_n = 0);
        return reduce(sum_mesh_values, result, "default"_n = i64{0});
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 1}, {2, 10}}, {{1, 3}, {2, 20}}}, {{{2, 1}}});

    CHECK(actual.equals<i64>({32, 46}));
}

TEST_CASE("a reduction observes mesh dependencies after switch outputs change",
          "[feature:mesh][feature:switch][feature:reduce][feature:nested-graph]"
          "[series:tsd][shape:deeply-nested][transition:update]") {
    const auto add_amount = node([](i64 value, int amount) { return value + amount; });

    const auto tested = [=](wp<TS<i64>> selected_case,
                            wp<TSD<i64, TS<i64>>> values,
                            wp<TSD<i64, TS<i64>>> dependencies) {
        const auto result = mesh(
            [=](auto, auto item, auto selector, auto dependency_key) {
                const auto value = add_mesh_dependency(item, mesh()[dependency_key]);
                return switch_(selector,
                               cases{case_(i64{1}, [=](auto input) { return add_amount(input, 10); }),
                                     case_(i64{2}, [=](auto input) { return add_amount(input, 20); })},
                               value);
            },
            values, pass_through(selected_case),
            dependencies, "__key_arg__"_n = 0);
        return reduce(sum_mesh_values, result, "default"_n = i64{0});
    };

    const auto actual = bg::run_graph(
        tested, {1, 2}, {{{1, 1}, {2, 10}}, {{2, 20}}}, {{{2, 1}}});

    CHECK(actual.equals<i64>({42, 82}));
}

} // namespace

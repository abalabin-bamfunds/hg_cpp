#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_graph.hpp>
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

const auto mesh_values = node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> values) {
    values[2].set(*value * 10);
    values[1].set(*value);
});

const auto mesh_dependencies =
    node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<std::int64_t>>> dependencies) { dependencies[2].set(1); });

const auto add_mesh_dependency = node([](In<TS<i64>> value, In<TS<i64>, optional> dependency, Out<TS<i64>> output) {
    output.set(*value + (dependency.valid() ? *dependency : 0));
});

const auto observe_mesh_dependency =
    node([](In<REF<TS<std::int64_t>, TimeSeriesGuarantee::deduplicated>, optional>) {});

const auto selectable_mesh_values = node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> output) {
    output[1].set(*value);
    output[2].set(*value * 10);
    output[3].set(*value * 100);
});

const auto removable_selected_mesh_values =
    node([](In<TS<i64>> value, In<TS<bool>> selected_present, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*selected_present) {
            output[1].set(*value);
        } else {
            output.erase(1);
        }
        output[2].set(*value * 10);
    });

const auto selectable_mesh_dependencies = node(
    [](In<TS<std::int64_t>> selected, Out<TSD<std::int64_t, TS<std::int64_t>>> output) { output[2].set(*selected); });

const auto ordered_mesh_values = node([](In<TS<std::int64_t>>, Out<TSD<std::int64_t, TS<i64>>> output) {
    output[2].set(20);
    output[1].set(10);
});

const auto optional_mesh_values =
    node([](In<TS<i64>> value, In<TS<bool>> present, Out<TSD<std::int64_t, TS<i64>>> output) {
        if (*present) {
            output[2].set(*value);
        } else {
            output.erase(2);
        }
    });

const auto optional_mesh_dependencies = node([](In<TS<bool>> present, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
    if (*present) {
        output[2].set(1);
    } else {
        output.erase(2);
    }
});

const auto dependent_branches = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
    output[2].set(20);
    output[3].set(30);
});

const auto transferred_mesh_dependency =
    node([](In<TS<bool>> first_dependent, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        if (*first_dependent) {
            output[2].set(1);
            output.erase(3);
        } else {
            output.erase(2);
            output[3].set(1);
        }
    });

const auto cyclic_mesh_dependencies =
    node([](In<TS<bool>>, bool self_cycle, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        output[1].set(self_cycle ? 1 : 2);
        if (!self_cycle) {
            output[2].set(1);
        }
    });

const auto diamond_first_dependencies = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
    output[1].set(2);
    output[2].set(4);
    output[3].set(4);
});

const auto diamond_second_dependencies =
    node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<std::int64_t>>> output) { output[1].set(3); });

const auto diamond_values = node([](In<TS<bool>>, Out<TSD<std::int64_t, TS<i64>>> output) {
    output[1].set(0);
    output[2].set(0);
    output[3].set(0);
    output[4].set(0);
});

struct CountMeshBranchEvaluation {
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;
    In<TS<i64>> value;
    In<TS<i64>, optional> first;
    In<TS<i64>, optional> second;
    Out<TS<i64>> output;

    inline static std::array<int, 5> counts{};

    void evaluate() const {
        ++counts.at(static_cast<std::size_t>(*key));
        output.set(static_cast<int>(*key) + (first.valid() ? *first : 0) + (second.valid() ? *second : 0));
    }
};

const auto count_mesh_branch_evaluation = node<CountMeshBranchEvaluation>();

struct StableMeshValues {
    In<TS<i64>> value;
    State<bool> initialized;
    Out<TSD<std::int64_t, TS<i64>>> output;

    void evaluate() const {
        if (!*initialized) {
            output[1].set(*value);
            *initialized = true;
        }
        output[2].set(*value * 10);
    }
};

const auto stable_mesh_values = node<StableMeshValues>();

struct CountSingleMeshDependency {
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;
    In<TS<i64>> value;
    In<TS<i64>, optional> dependency;
    Out<TS<i64>> output;

    inline static std::array<int, 3> counts{};

    void evaluate() const {
        ++counts.at(static_cast<std::size_t>(*key));
        output.set(*value + (dependency.valid() ? *dependency : 0));
    }
};

const auto count_single_mesh_dependency = node<CountSingleMeshDependency>();

TEST_CASE("mesh forwards optional positional input only to an optional branch parameter",
          "[feature:mesh][feature:nested-graph][feature:operator][feature:wiring][series:tsd]"
          "[validity:optional][definition:function]") {
    const auto dictionaries = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
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

const auto values_at_key_five =
    node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> output) { output[5].set(*value); });

const auto optional_inner_mesh_values = node([](In<TS<bool>> present, Out<TSD<std::int64_t, TS<i64>>> output) {
    if (*present) {
        output[1].set(0);
        output[2].set(0);
    } else {
        output.erase(1);
        output.erase(2);
    }
});

const auto increment_mesh_value = node([](In<TS<i64>> value, Out<TS<i64>> output) { output.set(*value + 1); });

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

const auto mesh_keys = node([](In<TS<bool>> present, Out<TSS<std::int64_t>> output) {
    if (*present) {
        output.insert(7);
    } else {
        output.erase(7);
    }
});

struct RecordMeshDependencyKey {
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;
    In<TS<std::int64_t>> dependency_key;
    Out<TS<std::int64_t>> output;

    inline static std::vector<int> events;

    void evaluate() const {
        events.push_back(static_cast<int>(*key * 10 + 1));
        output.set(*dependency_key);
    }
};

const auto record_mesh_dependency_key = node<RecordMeshDependencyKey>();

struct RecordMeshCompletion {
    In<TS<std::int64_t, TimeSeriesGuarantee::const_>> key;
    In<TS<i64>> value;
    In<TS<std::int64_t>, optional> dependency;
    Out<TS<std::int64_t>> output;

    void evaluate() const {
        RecordMeshDependencyKey::events.push_back(static_cast<int>(*key * 10 + 2));
        output.set(*key + *value * 0 + (dependency.valid() ? *dependency : 0));
    }
};

const auto record_mesh_completion = node<RecordMeshCompletion>();

struct MeshSink {
    In<TS<i64>> value;

    inline static std::vector<int> values;

    void evaluate() const { values.push_back(*value); }
};

const auto mesh_sink = node<MeshSink>();

TEST_CASE("mesh evaluates a dependency branch before the branch that depends on it",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[binding:reference-to-value][transition:addition]") {
    const auto tested = [](wp<TS<i64>> value) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 mesh_values(value), mesh_dependencies(value));
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 3});

    CHECK(actual.equals<bg::i64>({11, 33}));
}

TEST_CASE("mesh resumes a dependency node after its dependency completes",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition]") {
    RecordMeshDependencyKey::events.clear();

    const auto tested = [](wp<TS<std::int64_t>> selected) {
        const auto result = mesh(
            [](auto key, auto value, auto dependency_key) {
                const auto recorded_key = record_mesh_dependency_key(key, dependency_key);
                return record_mesh_completion(key, value, mesh()[recorded_key]);
            },
            ordered_mesh_values(selected), selectable_mesh_dependencies(selected), "__key_arg__"_n = 0);
        return result[std::int64_t{2}].as<TS<std::int64_t>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 1>{1});

    CHECK(actual.equals<std::int64_t>({3}));
    CHECK(RecordMeshDependencyKey::events == std::vector{21, 12, 22});
}

TEST_CASE("mesh creates a branch for a requested missing key",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition]") {
    const auto tested = [](wp<TS<std::int64_t>> selected) {
        const auto result = mesh(
            [](auto key, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            selectable_mesh_dependencies(selected), "__key_arg__"_n = 0);
        return result[std::int64_t{1}].as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                                              TimeSeriesGuarantee::ordinary>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 1>{1});

    CHECK(actual.equals<std::int64_t>({1}));
}

TEST_CASE("mesh follows a time-series dependency key",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][series:value]"
          "[shape:nested][transition:update]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<std::int64_t>> selected) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 selectable_mesh_values(value), selectable_mesh_dependencies(selected));
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2}, std::array<std::int64_t, 2>{1, 3});

    CHECK(actual.equals<bg::i64>({11, 220}));
}

TEST_CASE("mesh invalidates a dependency when the dependency branch output becomes invalid",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<bool>> selected_present) {
        const auto result =
            mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                 removable_selected_mesh_values(value, selected_present), mesh_dependencies(value));
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2}, std::array{true, false});

    CHECK(actual.equals<bg::i64>({11, 20}));
}

TEST_CASE("mesh retains a dependency-only branch until its last dependent is removed",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:addition][transition:removal]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<bool>> present) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            optional_mesh_values(value, present), optional_mesh_dependencies(present), "__key_arg__"_n = 0);
        return result[std::int64_t{1}].as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                                              TimeSeriesGuarantee::ordinary>>();
    };

    const auto actual = testing::test_graph(tested, std::array{7, 8, 9}, std::array{true, false, true});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt, 1}));
}

TEST_CASE("mesh destroys removed dependent branches and their dependency branches",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:removal][transition:lifecycle]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<bool>> present) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            optional_mesh_values(value, present), optional_mesh_dependencies(present), "__key_arg__"_n = 0);
        return result[std::int64_t{1}].as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                                              TimeSeriesGuarantee::ordinary>>();
    };

    const auto actual = testing::test_graph(tested, std::array{7, 8, 9}, std::array{true, false, false});

    CHECK(actual.equals<std::int64_t>({1, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh destroys a removed dependency while its dependent remains",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation][transition:lifecycle]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<bool>> selected) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            optional_mesh_values(value, operators::const_(true)), optional_mesh_dependencies(selected),
            "__key_arg__"_n = 0);
        return result[std::int64_t{2}].as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                                              TimeSeriesGuarantee::ordinary>>();
    };

    const auto actual = testing::test_graph(tested, std::array{7, 8, 9}, std::array{true, false, false});

    CHECK(actual.equals<std::int64_t>({2, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh transfers a dependency after its previous dependent releases it",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:invalidation][transition:revalidation][transition:lifecycle]") {
    const auto tested = [](wp<TS<bool>> first_dependent) {
        const auto result = mesh(
            [](auto key, auto, auto dependency_key) {
                observe_mesh_dependency(mesh()[dependency_key]);
                return key;
            },
            dependent_branches(first_dependent), transferred_mesh_dependency(first_dependent), "__key_arg__"_n = 0);
        return result[std::int64_t{3}].as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>,
                                              TimeSeriesGuarantee::ordinary>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true, false, false});

    CHECK(actual.equals<std::int64_t>({3, std::nullopt, std::nullopt}));
}

TEST_CASE("mesh rejects self and multi-branch dependency cycles",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]") {
    const auto tested = [](bool self_cycle) {
        return [=](wp<TS<bool>> trigger) {
            const auto result =
                mesh([](auto item, auto dependency_key) { return add_mesh_dependency(item, mesh()[dependency_key]); },
                     optional_mesh_values(1, trigger), cyclic_mesh_dependencies(trigger, self_cycle));
            return result[std::int64_t{1}].as<TS<i64>>();
        };
    };

    CHECK_THROWS_AS(testing::test_graph(tested(true), std::array{true}), std::runtime_error);
    CHECK_THROWS_AS(testing::test_graph(tested(false), std::array{true}), std::runtime_error);
}

TEST_CASE("mesh evaluates a shared dependency once",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[shape:diamond]") {
    CountMeshBranchEvaluation::counts.fill(0);

    const auto tested = [](wp<TS<bool>> trigger) {
        const auto result = mesh(
            [](auto key, auto value, auto first_key, auto second_key) {
                return count_mesh_branch_evaluation(key, value, mesh()[first_key], mesh()[second_key]);
            },
            diamond_values(trigger), diamond_first_dependencies(trigger), diamond_second_dependencies(trigger),
            "__key_arg__"_n = 0);
        return result[std::int64_t{1}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{true});

    CHECK(actual.equals<bg::i64>({14}));
    CHECK(CountMeshBranchEvaluation::counts[4] == 1);
}

TEST_CASE("mesh does not reevaluate a clean dependency branch",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tsd][shape:nested]"
          "[transition:update]") {
    CountSingleMeshDependency::counts.fill(0);

    const auto tested = [](wp<TS<i64>> value) {
        const auto result =
            mesh([](auto key, auto item,
                    auto dependency_key) { return count_single_mesh_dependency(key, item, mesh()[dependency_key]); },
                 stable_mesh_values(value), mesh_dependencies(value), "__key_arg__"_n = 0);
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({11, 21}));
    CHECK(CountSingleMeshDependency::counts[1] == 1);
    CHECK(CountSingleMeshDependency::counts[2] == 2);
}

TEST_CASE("mesh runs inside map", "[feature:mesh][feature:map][feature:nested-graph][feature:operator][series:tsd]"
                                  "[shape:doubly-nested]") {
    const auto tested = [](wp<TS<i64>> value) {
        const auto outer = map(
            [](auto item) {
                const auto result = mesh(
                    [](auto inner, auto dependency_key) { return add_mesh_dependency(inner, mesh()[dependency_key]); },
                    mesh_values(item), mesh_dependencies(item));
                return result[std::int64_t{2}].template as<TS<i64>>();
            },
            values_at_key_five(value));
        return outer[std::int64_t{5}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 3});

    CHECK(actual.equals<bg::i64>({11, 33}));
}

TEST_CASE("map runs inside mesh", "[feature:mesh][feature:map][feature:nested-graph][feature:operator][series:tsd]"
                                  "[shape:doubly-nested]") {
    const auto tested = [](wp<TS<i64>> value) {
        const auto outer = mesh(
            [](auto item) {
                const auto result =
                    map([](auto inner) { return increment_mesh_value(inner); }, values_at_key_five(item));
                return result[std::int64_t{5}].template as<TS<i64>>();
            },
            values_at_key_five(value));
        return outer[std::int64_t{5}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 3});

    CHECK(actual.equals<bg::i64>({2, 4}));
}

TEST_CASE("a named outer mesh is selected through an inner mesh and switch",
          "[feature:mesh][feature:switch][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][transition:addition]") {
    const auto tested = [](wp<TS<i64>> value) {
        const auto outer = mesh(
            "name"_n = "outer",
            [](auto key, auto item) {
                return switch_(key,
                               cases{
                                   case_(std::int64_t{1}, [](auto leaf) { return leaf; }),
                                   case_(std::int64_t{2},
                                         [](auto selected) {
                                             const auto inner = mesh(
                                                 "name"_n = "inner",
                                                 [](auto inner_value) {
                                                     return add_mesh_dependency(inner_value,
                                                                                mesh("outer")[std::int64_t{1}]);
                                                 },
                                                 values_at_key_five(selected));
                                             return inner[std::int64_t{5}].template as<TS<i64>>();
                                         }),
                               },
                               item)
                    .template as<TS<i64>>();
            },
            mesh_values(value), "__key_arg__"_n = 0);
        return outer[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{2, 3});

    REQUIRE(actual.size() == 2);
    REQUIRE(actual[0]);
    REQUIRE(actual[1]);
    CHECK(actual[0]->value() == 22);
    CHECK(actual[1]->value() == 33);
}

TEST_CASE("a mesh dependency follows switch output replacement",
          "[feature:mesh][feature:switch][feature:nested-graph][feature:operator][feature:reference]"
          "[series:tsd][shape:deeply-nested][binding:reference-to-value][transition:update]"
          "[transition:lifecycle]") {
    const auto add_amount = node([](In<TS<i64>> value, int amount, Out<TS<i64>> output) {
        output.set(*value + amount);
    });

    const auto tested = [&](wp<TS<std::int64_t>> selected_case, wp<TS<i64>> value) {
        const auto result = mesh(
            [&](auto key, auto item, auto selected) {
                return switch_(
                           key,
                           cases{
                               case_(std::int64_t{1},
                                     [&](auto branch_value, auto branch_selected) {
                                         return switch_(
                                                    branch_selected,
                                                    cases{
                                                        case_(std::int64_t{1},
                                                              [&](auto selected_value) {
                                                                  return add_amount(selected_value, 100);
                                                              }),
                                                        case_(std::int64_t{2},
                                                              [&](auto selected_value) {
                                                                  return add_amount(selected_value, 200);
                                                              }),
                                                    },
                                                    branch_value)
                                             .template as<TS<i64>>();
                                     }),
                               case_(std::int64_t{2},
                                     [](auto branch_value, auto) {
                                         return add_mesh_dependency(branch_value, mesh()[std::int64_t{1}]);
                                     }),
                           },
                           item, selected)
                    .template as<TS<i64>>();
            },
            mesh_values(value), pass_through(selected_case), "__key_arg__"_n = 0);
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array<std::int64_t, 4>{1, 2, 2, 1},
                                            std::array{1, 2, 3, 4});

    REQUIRE(actual.size() == 4);
    REQUIRE(actual[0]);
    REQUIRE(actual[1]);
    REQUIRE(actual[2]);
    REQUIRE(actual[3]);
    CHECK(actual[0]->value() == 111);
    CHECK(actual[1]->value() == 222);
    CHECK(actual[2]->value() == 233);
    CHECK(actual[3]->value() == 144);
}

TEST_CASE("runtime map instances keep independent outer mesh dependencies",
          "[feature:mesh][feature:map][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][shape:multiple]") {
    const auto inner_values = node([](In<TS<i64>> value, Out<TSD<std::int64_t, TS<i64>>> output) {
        output[10].set(*value);
        output[20].set(*value * 2);
    });
    const auto inner_dependencies = node([](In<TS<i64>>, Out<TSD<std::int64_t, TS<std::int64_t>>> output) {
        output[10].set(1);
        output[20].set(2);
    });

    const auto tested = [&](wp<TS<i64>> value) {
        const auto outer = mesh(
            "name"_n = "outer",
            [&](auto key, auto item) {
                return switch_(key,
                               cases{
                                   case_(std::tuple{std::int64_t{1}, std::int64_t{2}}, [](auto leaf) { return leaf; }),
                                   case_(std::int64_t{3},
                                         [&](auto selected) {
                                             const auto mapped = map(
                                                 [](auto inner, auto dependency) {
                                                     return add_mesh_dependency(inner, mesh("outer")[dependency]);
                                                 },
                                                 inner_values(selected), inner_dependencies(selected));
                                             return mapped[std::int64_t{20}].template as<TS<i64>>();
                                         }),
                               },
                               item)
                    .template as<TS<i64>>();
            },
            selectable_mesh_values(value), "__key_arg__"_n = 0);
        return outer[std::int64_t{3}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2});

    CHECK(actual.equals<bg::i64>({210, 420}));
}

TEST_CASE("a map removes and recreates branches after pending mesh dependencies",
          "[feature:mesh][feature:map][feature:nested-graph][feature:operator]"
          "[series:tsd][shape:deeply-nested][transition:removal][transition:addition]") {
    const auto tested = [](wp<TS<i64>> value, wp<TS<bool>> present) {
        const auto outer = mesh(
            "name"_n = "outer",
            [&](auto key, auto item, auto branch_present) {
                return switch_(key,
                               cases{
                                   case_(std::tuple{std::int64_t{1}, std::int64_t{2}},
                                         [](auto leaf, auto) { return leaf; }),
                                   case_(std::int64_t{3},
                                         [](auto, auto selected_present) {
                                             const auto selected = map(
                                                 [](auto selected_key, auto inner_value) {
                                                     return add_mesh_dependency(inner_value,
                                                                                mesh("outer")[selected_key]);
                                                 },
                                                 optional_inner_mesh_values(selected_present), "__key_arg__"_n = 0);
                                             return selected[std::int64_t{2}].template as<TS<i64>>();
                                         }),
                               },
                               item, branch_present)
                    .template as<TS<i64>>();
            },
            selectable_mesh_values(value), pass_through(present), "__key_arg__"_n = 0);
        return outer[std::int64_t{3}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1, 2, 3}, std::array{true, false, true});

    CHECK(actual.equals<bg::i64>({10, std::nullopt, 30}));
}

TEST_CASE("a mesh dependency branch reports timed work through its owner",
          "[feature:mesh][feature:nested-graph][feature:operator][feature:timed-scheduling]"
          "[series:tsd][shape:nested]") {
    const auto tested = [](wp<TS<i64>> value) {
        const auto result = mesh(
            [](auto item, auto dependency_key) {
                return add_mesh_dependency(delayed_mesh_value(item), mesh()[dependency_key]);
            },
            mesh_values(value), mesh_dependencies(value));
        return result[std::int64_t{2}].as<TS<i64>>();
    };

    const auto actual = testing::test_graph(tested, std::array{1});

    REQUIRE(actual.size() == 3);
    CHECK_FALSE(actual[0]);
    CHECK_FALSE(actual[1]);
    REQUIRE(actual[2]);
    CHECK(actual[2]->value() == 11);
}

TEST_CASE("mesh supports source and sink graphs",
          "[feature:mesh][feature:nested-graph][feature:operator][series:tss][series:tsd]"
          "[shape:source][shape:sink]") {
    MeshSink::values.clear();

    const auto source_test = [](wp<TS<bool>> present) {
        return mesh([](auto key) { return key; }, mesh_keys(present))[std::int64_t{7}]
            .as<REF<TS<std::int64_t, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::ordinary>>();
    };
    const auto source_result = testing::test_graph(source_test, std::array{true, false, true});

    CHECK(source_result.equals<std::int64_t>({7, std::nullopt, 7}));

    const auto sink_test = [](wp<TS<i64>> value) -> wp<TS<i64>> {
        mesh([](auto item) { mesh_sink(item); }, values_at_key_five(value));
        return value;
    };
    const auto sink_result = testing::test_graph(sink_test, std::array{1, 2});

    CHECK(sink_result.equals<bg::i64>({1, 2}));
    CHECK(MeshSink::values == std::vector{1, 2});
}

} // namespace

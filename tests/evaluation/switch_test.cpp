#include <bg/graph.hpp>
#include <bg/map.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/reduce.hpp>
#include <bg/run_graph.hpp>
#include <bg/switch.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tsl/tsl_output.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <tuple>

namespace {

using namespace bg;
using namespace bg::literals;

const auto add = node([](i64 value, int amount) { return value + amount; });

TEST_CASE("switch selects a case graph from its key",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:function]") {
    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(i64{1}, [](auto input) { return add(input, 10); }),
                       case_(i64{2}, [](auto input) { return add(input, 20); }),
                   },
                   value);
    };

    const auto actual = bg::run_graph(tested, {1, 2, 2, 1}, {3, 3, 4, 5});

    CHECK(actual.equals<i64>({13, 23, 24, 15}));
}

TEST_CASE("switch retains node templates captured by a generic case function",
          "[feature:switch][feature:nested-graph][feature:operator][series:value]"
          "[definition:function][lifetime:capture]") {
    const auto increment = node([](i64 value) { return value + 1; });

    const auto tested = [increment](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(key, cases{case_(i64{1}, [increment](auto input) { return increment(input); })}, value);
    };

    CHECK(bg::run_graph(tested, {1, 1}, {3, 4}).equals<i64>({4, 5}));
}

TEST_CASE("switch retains node templates captured by a move-only graph case",
          "[feature:switch][feature:nested-graph][feature:operator][series:value]"
          "[definition:graph][lifetime:capture][lifetime:move-only]") {
    const auto increment = node([](i64 value) { return value + 1; });

    const auto tested = [increment](wp<TS<i64>> key, wp<TS<i64>> value) {
        auto branch = graph("name"_n = "move_only_switch_case",
                            [increment, amount = std::make_unique<int>(1)](wp<TS<i64>> input) {
                                return add(increment(input), *amount);
                            });

        return switch_(key, cases{case_(i64{1}, std::move(branch))}, value);
    };

    CHECK(bg::run_graph(tested, {1, 1}, {3, 4}).equals<i64>({5, 6}));
}

TEST_CASE("switch holds fixed list items across free and native branches",
          "[feature:switch][feature:reference][series:fixed-tsl][representation:free]"
          "[representation:native][transition:update]") {
    const auto pack = node([](In<TS<i64>> first, In<TS<i64>> second,
                              Out<TSL<TS<i64>, 2>> output) {
        if (first.modified()) {
            output[0].set(*first);
        }
        if (second.modified()) {
            output[1].set(*second);
        }
    });

    const auto native = graph("name"_n = "native_fixed_list",
                              [&](wp<TS<i64>> first, wp<TS<i64>> second) { return pack(first, second); });

    const auto free = graph("name"_n = "free_fixed_list",
                            [](wp<TS<i64>> first, wp<TS<i64>> second) { return tsl(first, second); });

    const auto sum = node([](In<TSL<TS<i64>, 2>> values) { return *values[0] + *values[1]; });

    const auto tested = [&](wp<TS<bool>> key, wp<TS<i64>> first, wp<TS<i64>> second) {
        const auto selected = switch_(key, cases{case_(true, free), case_(false, native)}, first, second);
        return sum(selected);
    };

    CHECK(bg::run_graph(tested,
                        {true, true, true, false, false, true},
                        {1, 2, literal::SKIP, literal::SKIP, 3, literal::SKIP},
                        {10, literal::SKIP, 20, literal::SKIP, literal::SKIP, 30})
              .equals<i64>({11, 12, 22, 22, 23, 33}));
}

TEST_CASE("switch output reaches reference consumers across dictionary branch changes",
          "[feature:switch][feature:reference][series:tsd][series:reference]"
          "[binding:reference-to-reference][binding:reference-to-value][transition:update]") {
    const auto reference_dictionary = node([](In<REF<TS<i64>>> value,
                                              Out<TSD<i64, REF<TS<i64>>>> output) {
        if (value.modified()) {
            output[1].copy(value);
        }
    });

    const auto value_dictionary = node([](In<TS<i64>> value, Out<TSD<i64, TS<i64>>> output) {
        if (value.modified()) {
            output[1].set(*value);
        }
    });

    const auto reference_branch = graph("name"_n = "reference_dictionary_branch",
                                        [&](wp<TS<i64>> value) { return reference_dictionary(value); });

    const auto value_branch = graph("name"_n = "value_dictionary_branch",
                                    [&](wp<TS<i64>> value) { return value_dictionary(value); });

    const auto forward = node([](In<REF<TSD<i64, REF<TS<i64>>>>> input,
                                 Out<REF<TSD<i64, REF<TS<i64>>>>> output) { output.copy(input); });

    const auto observe = node([](In<TSD<i64, TS<i64>>> input) {
        return input.contains(1) ? *input[1] : i64{-1};
    });

    const auto tested = [&](wp<TS<bool>> select_reference, wp<TS<i64>> value) {
        const auto selected = switch_(select_reference,
                                      cases{case_(true, reference_branch), case_(false, value_branch)}, value);
        return observe(forward(selected));
    };

    CHECK(bg::run_graph(tested,
                        {true, literal::SKIP, false, literal::SKIP, true, literal::SKIP},
                        {10, 20, literal::SKIP, 30, literal::SKIP, 40})
              .equals<i64>({10, 20, 20, 30, 30, 40}));
}

struct StatefulCase {
    In<TS<i64>> value;
    State<int> instance;
    Out<TS<i64>> output;

    void start() const { *instance = ++start_count; }
    void evaluate() const { output.set(*instance * 100 + *value); }

    inline static int start_count{};
};

const auto stateful_case = node<StatefulCase>();

const auto optional_key = node([](i64 value, Out<TS<i64>> output) {
    if (value < 0) {
        output.invalidate();
    } else {
        output.set(value);
    }
});

struct DelayedCase {
    static constexpr bool uses_timed_scheduling = true;

    Out<TS<i64>> output;

    void start(EvaluationContext *context) const {
        context->schedule_at(context->evaluation_time() + std::chrono::microseconds{2});
    }

    void evaluate() const {
        ++evaluation_count;
        output.set(17);
    }

    inline static int evaluation_count{};
};

const auto delayed_case = node<DelayedCase>();

struct MappedSwitchCase {
    In<TS<i64>> trigger;
    int offset;
    State<int> evaluation_count;
    Out<TS<i64>> output;

    void start() const { *evaluation_count = 0; }

    void evaluate() const { output.set(offset + ++*evaluation_count); }
};

const auto mapped_switch_case = node<MappedSwitchCase>();

const auto add_mapped_results = node([](std::optional<i64> first,
                                        std::optional<i64> second,
                                        Out<TS<i64>> output) {
    if (first && second) {
        output.set(*first + *second);
    } else {
        output.invalidate();
    }
});

const auto mapped_switch = graph(
    "name"_n = "mapped_switch",
    [](wp<TS<i64>> trigger, wp<TS<i64>> selector) {
        return switch_(
                   selector,
                   cases{
                       case_(1, [](auto value) { return mapped_switch_case(value, 10); }),
                       case_(2, [](auto value) { return mapped_switch_case(value, 20); }),
                   },
                   trigger)
            .as<TS<i64>>();
    });

TEST_CASE("switch releases cached string graph arguments with its root wiring",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][scalar:string]") {
    const auto identity = graph("name"_n = "string_identity", [](wp<TS<string>> value) { return value; });

    const auto fallback = graph("name"_n = "string_fallback", [](wp<TS<string>>) {
        return operators::const_(std::string_view{"fallback"})
            .as<TS<string, TimeSeriesGuarantee::const_>>();
    });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<string>> value) {
        return switch_(key, cases{case_(i64{1}, identity), case_(DEFAULT, fallback)}, value);
    };

    const auto actual = bg::run_graph(
        tested, {1, 0}, {std::string_view{"original"}, literal::SKIP});

    CHECK(actual.equals<string>({"original", "fallback"}));
}

TEST_CASE("switch accepts an optional wiring argument as its key",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][validity:optional][definition:function]") {
    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(opt(key), cases{case_(1, [](auto input) { return add(input, 10); })}, value);
    };

    const auto actual = bg::run_graph(tested, {1, 1}, {3, 4});

    CHECK(actual.equals<i64>({13, 14}));
}

TEST_CASE("switch uses DEFAULT for invalid and unmatched keys",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto selected = graph("name"_n = "selected", [](wp<TS<i64>> value) {
        return add(value, 10);
    });

    const auto fallback = graph("name"_n = "fallback", [](wp<TS<i64>> value) {
        return add(value, 100);
    });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(key,
                       cases{case_(i64{1}, selected), case_(DEFAULT, fallback)},
                       value);
    };

    const auto actual = bg::run_graph(tested, {1, 9}, {2, 3});

    CHECK(actual.equals<i64>({12, 103}));
}

TEST_CASE("switch reports an unmatched or invalid key without DEFAULT",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][feature:validity][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(key,
                       cases{case_(i64{1}, [](auto input) { return add(input, 1); })},
                       value);
    };

    CHECK_THROWS_WITH(
        (void)bg::run_graph(tested, {1, 9}, {5, 6}),
        "switch_ key does not match a case and no DEFAULT case is defined");

    const auto invalid_tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(optional_key(key),
                       cases{case_(i64{1}, [](auto input) { return add(input, 1); })},
                       value);
    };
    CHECK_THROWS_WITH(
        (void)bg::run_graph(invalid_tested, {-1}, {5}),
        "switch_ key does not match a case and no DEFAULT case is defined");
}

TEST_CASE("an outputless DEFAULT explicitly invalidates a switch output",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[shape:sink][series:value][transition:invalidation][definition:function]") {
    auto recorded_values = std::vector<i64>{};

    const auto recording_sink = node([&recorded_values](i64 value) { recorded_values.push_back(value); });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(DEFAULT, [&](auto input) { recording_sink(input); }),
                       case_(1, [](auto input) { return add(input, 10); }),
                   },
                   value);
    };

    const auto actual = bg::run_graph(tested, {1, 9}, {2, 3});

    CHECK(actual.equals<i64>({12, std::nullopt}));
    CHECK(recorded_values == std::vector<i64>{3});
}

TEST_CASE("switch matches named graph arguments and omits unused named arguments",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][definition:aggregate]") {
    struct SelectedArgumentGraph {
        wp<TS<i64>> selected;

        auto wire() const { return add(selected, 1); }
    };

    const auto selected = graph<SelectedArgumentGraph>();

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value,
                            wp<TS<i64>> unused) {
        return switch_(key, cases{case_(i64{1}, selected)},
                       "selected"_n = value, "unused"_n = unused);
    };

    const auto actual = bg::run_graph(
        tested, {1, 1}, {3, 5}, {40, 60});

    CHECK(actual.equals<i64>({4, 6}));
}

TEST_CASE("switch forwards positional arguments to a variadic case graph",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][series:tsb][schema:generic][definition:function]") {
    const auto select_second = graph(
        "name"_n = "select_second_switch_argument",
        [](wp<TSB<SCHEMA>, var_args> arguments) {
            return wp<TS<i64>>{arguments[1].time_series()};
        });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> first,
                            wp<TS<i64>> second) {
        return switch_(key, cases{case_(1, select_second)}, first, second);
    };

    const auto actual = bg::run_graph(
        tested, {1, 1}, {3, 5}, {40, 60});

    CHECK(actual.equals<i64>({40, 60}));
}

TEST_CASE("switch forwards named arguments to a variadic case graph",
          "[feature:switch][feature:nested-graph][feature:operator][feature:variadic-wiring]"
          "[shape:nested][series:value][series:tsb][schema:generic][definition:function]") {
    const auto select_item = graph(
        "name"_n = "select_named_switch_argument",
        [](wp<TSB<SCHEMA>, var_kwargs> arguments) {
            return wp<TS<i64>>{arguments["item"].time_series()};
        });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(key, cases{case_(1, select_item)}, "item"_n = value);
    };

    const auto actual = bg::run_graph(
        tested, {1, 1}, {3, 5});

    CHECK(actual.equals<i64>({3, 5}));
}

TEST_CASE("switch captures scalar case arguments at wiring time",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:function]") {
    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{case_(1, [](auto input, int amount) { return add(input, amount); })},
                   value, 10);
    };

    const auto actual = bg::run_graph(
        tested, {1, 1}, {3, 5});

    CHECK(actual.equals<i64>({13, 15}));
}

struct switch_graph_boundary {
    static constexpr bool deduplicate_wiring = false;
    Out<TS<i64>> output;
    void evaluate() const {}
};

TEST_CASE("switch binds graph arguments to cases without forwarding them to overload selection",
          "[feature:switch][feature:nested-graph][feature:graph-argument][feature:wiring]") {
    auto graph_wiring = GraphWiring{};
    const auto graph_context = WiringContext{graph_wiring};
    const auto graph_boundary = node<switch_graph_boundary>();
    const auto graph_input = graph_boundary();
    const auto graph_boundaries = std::array<WiringPoint, 1>{graph_input};
    (void)graph_wiring.complete_pending_wiring();
    const auto graph_argument = GraphArgument{
        details::graph_wiring_structure(graph_wiring, graph_boundaries, static_cast<WiringPoint>(graph_input)),
        std::make_shared<const int>(1)};
    auto received_graph_argument = false;
    const auto branch = graph(
        "name"_n = "switch_graph_argument_branch",
        [&](wp<TS<i64>> value, GraphArgument function) {
            const auto payload = function.payload<int>();
            received_graph_argument = function.structure().boundary_count() == 1 && payload && *payload == 1;
            return add(value, 3);
        });

    const auto typed = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(key, cases{case_(1, branch)}, value, graph_argument).as<TS<i64>>();
    };
    CHECK(bg::run_graph(typed, {1, 1}, {2, 5}).equals<i64>({5, 8}));
    CHECK(received_graph_argument);
    received_graph_argument = false;

    const auto runtime = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        const auto keys = std::array{WiringArgument{i64{1}}};
        const auto switch_cases = std::array{SwitchCaseArgument{
            std::span<const WiringArgument>{keys}, branch}};
        const auto arguments = std::array{
            WiringArgument{value}, WiringArgument{graph_argument}};
        return switch_(static_cast<WiringPoint>(key),
                       std::span<const SwitchCaseArgument>{switch_cases},
                       std::span<const WiringArgument>{arguments})
            .as<TS<i64>>();
    };
    CHECK(bg::run_graph(runtime, {1, 1}, {2, 5}).equals<i64>({5, 8}));
    CHECK(received_graph_argument);
}

TEST_CASE("equal switch key ticks retain inner graph state and returning recreates it",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:update][transition:removal][definition:function]") {
    StatefulCase::start_count = 0;

    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(i64{1}, [](auto input) { return stateful_case(input); }),
                       case_(i64{2}, [](auto input) { return stateful_case(input); }),
                   },
                   value);
    };

    const auto actual = bg::run_graph(tested, {1, 1, 2, 1}, {1, 2, 3, 4});

    CHECK(actual.equals<i64>({101, 102, 203, 304}));
    CHECK(StatefulCase::start_count == 3);
}

TEST_CASE("switching between tuple keys recreates the case graph",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:update][transition:removal][definition:function]") {
    StatefulCase::start_count = 0;

    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(
                   key,
                   cases{
                       case_(std::tuple{1, 2},
                             [](auto input) { return stateful_case(input); }),
                   },
                   value);
    };

    const auto actual = bg::run_graph(tested, {1, 1, 2, 2}, {1, 2, 3, 4});

    CHECK(actual.equals<i64>({101, 102, 203, 204}));
    CHECK(StatefulCase::start_count == 2);
}

TEST_CASE("switch supports source and sink case graphs",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[shape:source][shape:sink][series:value][definition:function]") {
    const auto source_tested = [](wp<TS<i64>> key) {
        return switch_(key,
                       cases{case_(i64{1}, []() -> wp<TS<i64,
                                                                  TimeSeriesGuarantee::const_>> {
                           return operators::const_(42).as<TS<i64,
                                                               TimeSeriesGuarantee::const_>>();
                       })});
    };
    CHECK(bg::run_graph(source_tested, {1})
              .equals<i64>({42}));

    auto recorded_values = std::vector<i64>{};

    const auto recording_sink = node([&recorded_values](i64 value) { recorded_values.push_back(value); });

    const auto sink_tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        (void)switch_(key,
                      cases{case_(i64{1}, [&](auto input) { recording_sink(input); })},
                      value);
        return value;
    };
    CHECK(bg::run_graph(sink_tested, {1, 1}, {7, 9})
              .equals<i64>({7, 9}));
    CHECK(recorded_values == std::vector<i64>{7, 9});
}

TEST_CASE("switch accepts fully runtime cases and arguments",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][definition:runtime]") {
    const auto branch = graph("name"_n = "runtime_switch_branch", [](wp<TS<i64>> value) {
        return add(value, 3);
    });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        const auto runtime_keys = std::array{
            WiringArgument{i64{1}}, WiringArgument{i64{2}}};
        const auto runtime_cases = std::array{
            SwitchCaseArgument{std::span<const WiringArgument>{runtime_keys}, branch},
        };
        const auto runtime_arguments = std::array{WiringArgument{value}};
        return switch_(static_cast<WiringPoint>(key),
                       std::span<const SwitchCaseArgument>{runtime_cases},
                       std::span<const WiringArgument>{runtime_arguments});
    };

    CHECK(bg::run_graph(tested, {1, 2}, {2, 5})
              .equals<i64>({5, 8}));
}

TEST_CASE("runtime switch preserves an optional key qualifier",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][validity:optional][definition:runtime]") {
    const auto branch = graph("name"_n = "optional_runtime_switch_branch", [](wp<TS<i64>> value) {
        return add(value, 3);
    });
    auto operation = operator_<auto(TS<i64>, GRAPH_T, TS<i64>) -> TS<i64>,
                                details::switch_operator_implementation>{"runtime_switch", {"key", "cases", "value"}};
    operation.overload(graph("name"_n = "runtime_switch_required_key",
                              [](wp<TS<i64>>, const GraphArgument &, wp<TS<i64>> value) {
                                  return add(value, 10);
                              }));
    operation.overload(graph("name"_n = "runtime_switch_optional_key",
                              [](wp<TS<i64>, optional>, const GraphArgument &, wp<TS<i64>> value) {
                                  return add(value, 20);
                              }));

    const auto combine = node([](i64 left, i64 right) { return left + right; });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value) {
        const auto runtime_keys = std::array{WiringArgument{i64{1}}};
        const auto runtime_cases = std::array{
            SwitchCaseArgument{std::span<const WiringArgument>{runtime_keys}, branch},
        };
        const auto runtime_arguments = std::array{WiringArgument{value}};
        const auto key_point = static_cast<WiringPoint>(key);
        const auto required = operation(key_point, std::span<const SwitchCaseArgument>{runtime_cases},
                                         std::span<const WiringArgument>{runtime_arguments});
        const auto optional = operation(WiringArgument{opt(key)},
                                        std::span<const SwitchCaseArgument>{runtime_cases},
                                        std::span<const WiringArgument>{runtime_arguments});
        return combine(required, optional);
    };

    CHECK(bg::run_graph(tested, {1, 1}, {2, 5})
              .equals<i64>({34, 40}));
}

TEST_CASE("switch forwards optional positional and named inputs only to optional branch parameters",
          "[feature:switch][feature:nested-graph][feature:operator][feature:wiring]"
          "[validity:optional][definition:function]") {
    const auto keys = node([](Out<TS<i64>>) {});

    const auto values = node([](Out<TS<i64>>) {});

    const auto optional_positional = graph(
        "name"_n = "optional_positional_switch_branch",
        [](wp<TS<i64>, optional> value) { return value; });

    const auto required_positional = graph(
        "name"_n = "required_positional_switch_branch",
        [](wp<TS<i64>> value) { return value; });

    const auto optional_named = graph(
        "name"_n = "optional_named_switch_branch",
        [](wp<TS<i64>, "value"_n, optional> value) { return value; });

    const auto required_named = graph(
        "name"_n = "required_named_switch_branch",
        [](wp<TS<i64>, "value"_n> value) { return value; });

    testing::test_wiring([&] {
        const auto key = keys();
        const auto value = values();

        CHECK_NOTHROW(switch_(key, cases{case_(1, optional_positional)}, opt(value)));
        CHECK_THROWS_AS(switch_(key, cases{case_(1, required_positional)}, opt(value)), TypeResolutionError);
        CHECK_NOTHROW(switch_(key, cases{case_(1, optional_named)}, "value"_n = opt(value)));
        CHECK_THROWS_AS(switch_(key, cases{case_(1, required_named)}, "value"_n = opt(value)),
                        TypeResolutionError);
    });
}

TEST_CASE("switch DEFAULT handles an invalid key",
          "[feature:switch][feature:nested-graph][feature:operator][shape:nested]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<i64>> key, wp<TS<i64>> value) {
        return switch_(optional_key(key),
                       cases{
                           case_(i64{1}, [](auto input) { return add(input, 10); }),
                           case_(DEFAULT, [](auto input) { return add(input, 100); }),
                       },
                       value);
    };

    CHECK(bg::run_graph(tested, {1, -1}, {2, 3})
              .equals<i64>({12, 103}));
}

TEST_CASE("switch starts a DEFAULT source when its key is initially invalid",
          "[feature:switch][feature:nested-graph][feature:operator][shape:source]"
          "[series:value][transition:invalidation][definition:function]") {
    const auto tested = [](wp<TS<i64>> key) {
        return switch_(
                   optional_key(key),
                   cases{case_(DEFAULT, [] {
                       return operators::const_(7).as<TS<i64,
                                                          TimeSeriesGuarantee::const_>>();
                   })});
    };

    CHECK(bg::run_graph(tested, {-1})
              .equals<i64>({7}));
}

TEST_CASE("a switch inner graph reports timed work through its owner",
          "[feature:switch][feature:nested-graph][feature:operator][feature:timed-scheduling]"
          "[shape:nested][shape:source][series:value][definition:function]") {
    DelayedCase::evaluation_count = 0;

    const auto branch = graph("name"_n = "delayed_switch_case",
                              []() -> wp<TS<i64>> { return delayed_case(); });

    run_graph([&] {
        const auto key = operators::const_(i64{1});
        (void)switch_(key, cases{case_(i64{1}, branch)});
    });

    CHECK(DelayedCase::evaluation_count == 1);
}

TEST_CASE("switches inside a map retain and replace state independently",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[shape:doubly-nested][series:value][series:tsd][transition:update]"
          "[definition:function]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> selectors) {
        const auto result = map(mapped_switch, values, selectors);
        return add_mapped_results(result[i64{1}], result[i64{2}]);
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 0}, {2, 0}}, {{1, 0}, {2, 0}}, {{1, 0}, {2, 0}}, {{1, 0}, {2, 0}}},
        {{{1, 1}, {2, 2}}, {{1, 1}, {2, 2}}, {{1, 2}, {2, 2}}, {{1, 2}, {2, 1}}});

    CHECK(actual.equals<i64>({32, 34, 44, 33}));
}

TEST_CASE("removing and recreating a map entry recreates its switch graph",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[shape:doubly-nested][series:value][series:tsd][transition:addition]"
          "[transition:removal][definition:function]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> selectors) {
        const auto result = map(mapped_switch, values, selectors);
        return result[i64{1}];
    };

    const auto actual = bg::run_graph(
        tested, {{{1, 0}}, {{1, 0}}, {{1, literal::REMOVE}}, {{1, 0}}},
        {{{1, 1}}, {{1, 1}}, {{1, literal::REMOVE}}, {{1, 1}}});

    CHECK(actual.equals<i64>({11, 12, std::nullopt, 11}));
}

TEST_CASE("an unmatched switch key inside a map fails the outer graph",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[feature:lifecycle][shape:doubly-nested][series:value][series:tsd]"
          "[definition:function]") {
    const auto tested = [](wp<TSD<i64, TS<i64>>> values,
                           wp<TSD<i64, TS<i64>>> selectors) {
        const auto result = map(mapped_switch, values, selectors);
        return result[i64{1}];
    };

    CHECK_THROWS_WITH(
        (void)bg::run_graph(tested, {{{1, 0}}}, {{{1, 9}}}),
        "switch_ key does not match a case and no DEFAULT case is defined");
}

TEST_CASE("timed scheduling propagates through a switch inside a map",
          "[feature:switch][feature:map][feature:nested-graph][feature:operator]"
          "[feature:timed-scheduling][shape:doubly-nested][shape:source]"
          "[series:value][series:tsd][definition:function]") {
    DelayedCase::evaluation_count = 0;

    const auto delayed_mapped_switch = graph(
        "name"_n = "delayed_mapped_switch",
        [](wp<TS<i64>> selector) {
            return switch_(
                       selector,
                       cases{case_(1, []() -> wp<TS<i64>> { return delayed_case(); })})
                .as<TS<i64>>();
        });

    const auto tested = graph("name"_n = "timed_map_switch", [&](wp<TSD<i64, TS<i64>>> selectors) {
        (void)map(delayed_mapped_switch, selectors);
    });
    (void)bg::run_graph(tested, {{{1, 1}}});

    CHECK(DelayedCase::evaluation_count == 1);
}

TEST_CASE("switch crosses an additional nested graph boundary",
          "[feature:switch][feature:nested-graph][feature:operator][shape:doubly-nested]"
          "[series:value][binding:value-to-reference][binding:reference-to-value]"
          "[definition:function]") {
    const auto inner = graph(
        "name"_n = "nested_switch",
        [](wp<TS<i64>> key, wp<TS<i64>> value) {
            return switch_(key,
                           cases{case_(1, [](auto input) { return add(input, 10); }),
                                 case_(2, [](auto input) { return add(input, 20); })},
                           value)
                .as<TS<i64>>();
        });

    const auto tested = [&](wp<TS<i64>> key, wp<TS<i64>> value,
                            wp<TS<bool>> run) {
        return nested(inner, run, key, value);
    };

    const auto actual = bg::run_graph(
        tested, {1, 2, 2, 1}, {3, 3, 4, 5}, {true, true, true, true});

    CHECK(actual.equals<i64>({13, 23, 24, 15}));
}

TEST_CASE("switch branches consume a reduced TSD value",
          "[feature:switch][feature:reduce][feature:nested-graph][shape:doubly-nested]"
          "[series:tsd][transition:update][definition:function]") {
    const auto sum_values = graph("name"_n = "sum_switch_values",
                                  [](wp<TS<i64>> left, wp<TS<i64>> right) { return left + right; });
    const auto tested = [=](wp<TS<i64>> key, wp<TSD<i64, TS<i64>>> values) {
        const auto total = reduce(sum_values, values, "default"_n = i64{0});
        return switch_(key,
                       cases{case_(i64{1}, [](auto value) { return add(value, 10); }),
                             case_(i64{2}, [](auto value) { return add(value, 20); })},
                       total);
    };

    const auto actual = bg::run_graph(tested, {1, 2, 2, 1}, {{}, {{1, 1}}, {{2, 2}}, {{1, 3}}});

    CHECK(actual.equals<i64>({10, 21, 23, 15}));
}

template <template <typename, auto...> typename Field> struct ReducedPair {
    Field<i64> first;
    Field<i64> second;
};

TEST_CASE("switch passes a reduced value through a free bundle argument",
          "[feature:switch][feature:reduce][feature:nested-graph][series:tsd][series:tsb]"
          "[schema:named][representation:free][shape:doubly-nested]") {
    const auto sum_values = graph("name"_n = "sum_switch_values",
                                  [](wp<TS<i64>> left, wp<TS<i64>> right) { return left + right; });
    const auto tested = [=](wp<TS<i64>> key, wp<TSD<i64, TS<i64>>> values) {
        const auto total = reduce(sum_values, values, "default"_n = i64{0});
        const auto arguments = bundle<TSB<ReducedPair>>(
            "first"_n = total, "second"_n = total);
        return switch_(key,
                       cases{case_(DEFAULT, [=](auto pair) {
                           return sum_values(pair->first, pair->second);
                       })},
                       arguments);
    };

    const auto actual = bg::run_graph(tested, {1, 1, 1}, {{{1, 2}}, {{2, 3}}, {{1, literal::REMOVE}}});

    CHECK(actual.equals<i64>({4, 10, 6}));
}

} // namespace

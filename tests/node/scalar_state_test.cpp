#include <bg/bg.hpp>
#include <bg/run_graph.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

using namespace bg;

namespace {

struct collection_state_node {
    State<List<i64>> list;
    State<Set<i64>> set;
    State<Dict<i64, i64>> dict;
    Out<TS<i64>> output;

    void start() const {
        list->append(1);
        list->append(3);
        list->set(1, 2);

        (void)set->insert(4);
        (void)set->insert(4);
        (void)set->insert(5);

        dict->insert_or_assign(6, 7);
        dict->insert_or_assign(6, 8);

        output.set(static_cast<i64>(list->size() + set->size() + dict->at(6)));
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("collection state is mutable during a node invocation", "[node][state][scalar][feature:state]") {
    const auto collection_state = node<collection_state_node>();

    const auto graph = [&] { return collection_state(); };

    CHECK(bg::run_graph(graph).equals<i64>({12}));
}

namespace {

struct collection_copy_node {
    Scalar<List<string>> list;
    Scalar<Set<string>> set;
    Scalar<Dict<string, string>> dict;
    Scalar<Dict<string, List<string>>> nested_dict;
    Scalar<Opaque<const char *>> list_item_data;
    Scalar<Opaque<const char *>> set_item_data;
    Scalar<Opaque<const char *>> dict_key_data;
    Scalar<Opaque<const char *>> dict_value_data;
    Scalar<Opaque<const char *>> nested_item_data;

    void start(EvaluationContext *context) const {
        auto set_item_matches = false;
        set.for_each([&](const auto item) { set_item_matches = item.value().data() == set_item_data.value(); });

        auto dict_key_matches = false;
        auto dict_value_matches = false;
        dict.for_each([&](const auto key, const auto value) {
            dict_key_matches = key.value().data() == dict_key_data.value();
            dict_value_matches = value.value().data() == dict_value_data.value();
        });

        const auto nested_item_matches = nested_dict.at("nested")[0].value().data() == nested_item_data.value();

        context->global_state().write([&](auto &values) {
            values.set(util::symbol{"list_item_matches"},
                       ScalarValue{list[0].value().data() == list_item_data.value()});
            values.set(util::symbol{"set_item_matches"}, ScalarValue{set_item_matches});
            values.set(util::symbol{"dict_key_matches"}, ScalarValue{dict_key_matches});
            values.set(util::symbol{"dict_value_matches"}, ScalarValue{dict_value_matches});
            values.set(util::symbol{"nested_item_matches"}, ScalarValue{nested_item_matches});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled graph collection copies retain their string items",
          "[node][scalar][scalar:list][scalar:set][scalar:dict][scalar:string]"
          "[feature:scalar-storage][feature:graph-compilation][feature:global-state][storage:variable]") {
    const auto collection_copy = node<collection_copy_node>();

    const auto list = ScalarValue<List<string>>{std::array<std::string_view, 1>{"list item"}};
    const auto set = ScalarValue<Set<string>>{std::array<std::string_view, 1>{"set item"}};
    const auto dict = ScalarValue<Dict<string, string>>{
        std::array{std::pair{std::string_view{"dict key"}, std::string_view{"dict value"}}}};
    const auto nested_items = ScalarValue<List<string>>{
        std::array<std::string_view, 1>{"nested item"}};
    const auto nested_dict = ScalarValue<Dict<string, List<string>>>{
        std::array{std::pair{std::string_view{"nested"}, nested_items.view()}}};
    const auto list_item_data = ScalarValue<Opaque<const char *>>{list.view()[0].value().data()};
    auto set_item_text = static_cast<const char *>(nullptr);
    set.view().for_each([&](const auto item) { set_item_text = item.value().data(); });
    auto dict_key_text = static_cast<const char *>(nullptr);
    auto dict_value_text = static_cast<const char *>(nullptr);
    dict.view().for_each([&](const auto key, const auto value) {
        dict_key_text = key.value().data();
        dict_value_text = value.value().data();
    });
    const auto nested_item_text = nested_dict.view().at("nested")[0].value().data();
    const auto set_item_data = ScalarValue<Opaque<const char *>>{set_item_text};
    const auto dict_key_data = ScalarValue<Opaque<const char *>>{dict_key_text};
    const auto dict_value_data = ScalarValue<Opaque<const char *>>{dict_value_text};
    const auto nested_item_data = ScalarValue<Opaque<const char *>>{nested_item_text};
    auto global_state = GlobalState{};

    run_graph(
        [&] {
            collection_copy(list, set, dict, nested_dict, list_item_data, set_item_data, dict_key_data,
                            dict_value_data, nested_item_data);
        },
        GraphRunOptions{.global_state = &global_state});

    global_state.read([](const auto &values) {
        CHECK(*scalar_cast<bool>(values.at("list_item_matches")));
        CHECK(*scalar_cast<bool>(values.at("set_item_matches")));
        CHECK(*scalar_cast<bool>(values.at("dict_key_matches")));
        CHECK(*scalar_cast<bool>(values.at("dict_value_matches")));
        CHECK(*scalar_cast<bool>(values.at("nested_item_matches")));
    });
}

TEST_CASE("a runtime graph releases collection state storage", "[node][state][scalar][allocation][feature:state]") {
    const auto collection_state = node<collection_state_node>();

    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        collection_state();
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = GlobalState{};
    {
        auto runtime = RuntimeGraph{graph, allocator{memory}, global_state};
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

namespace {

struct generic_list_state_node {
    Scalar<List<SCALAR>> values;
    State<List<SCALAR>> state;
    Out<TS<SCALAR>> output;

    void start() const {
        state->append(values[0]);
        state->append(values[1]);
        state->set(0, values[1]);
        state->erase(1);

        CHECK(state->size() == 1);
        CHECK(state.value().size() == 1);
        output.set_value(state[0]);
    }

    void evaluate() const {}
};

using K = ScalarVariable<"K", Hashable>;
using V = ScalarVariable<"V">;
using TEXT = ScalarVariable<"TEXT", string, EqualityComparable>;
using OPAQUE = ScalarVariable<"OPAQUE", Opaque<std::string>>;

struct generic_scalar_state_node {
    In<TS<TEXT>, optional> input;
    State<TEXT> state;
    Out<TS<TEXT>> output;

    void evaluate() const {
        if (!input.valid()) {
            state.reset();
            output.invalidate();
            return;
        }

        state.set(input.value_view());
        output.set_value(state.value());
    }
};

struct generic_set_state_value_node {
    Scalar<Set<SCALAR>> values;
    State<Set<SCALAR>> state;
    Out<TS<SCALAR>> output;

    void start() const {
        values.for_each([&](const auto &value) {
            (void)state->insert(value);
            output.set_value(value);
        });
        CHECK(state.value().size() == values.size());
    }

    void evaluate() const {}
};

struct generic_dict_state_value_node {
    Scalar<Dict<SCALAR, SCALAR>> values;
    State<Dict<SCALAR, SCALAR>> state;
    Out<TS<SCALAR>> output;

    void start() const {
        values.for_each([&](auto key, auto mapped) {
            state.insert_or_assign(key, mapped);
            output.set_value(mapped);
        });
        CHECK(state.value().size() == values.size());
    }

    void evaluate() const {}
};

struct unsupported_scalar_state_node {
    Scalar<OPAQUE> input;
    State<OPAQUE> state;

    void start() const { state.set(input); }
    void evaluate() const {}
};

} // namespace

TEST_CASE("generic collection state uses its resolved storage",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:generic]") {
    const auto generic_list_state = node<generic_list_state_node>();

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{3, 4}};

    const auto graph = [&] { return generic_list_state(input); };

    CHECK(bg::run_graph(graph).equals<i64>({4}));
}

TEST_CASE("generic set state exposes a borrowed scalar value",
          "[node][state][scalar][generic][collection][feature:state]") {
    const auto state_node = node<generic_set_state_value_node>();
    const auto input = ScalarValue<Set<i64>>{std::array<i64, 1>{7}};
    const auto graph = [&] { return state_node(input); };

    CHECK(bg::run_graph(graph).equals<i64>({7}));
}

TEST_CASE("generic dictionary state exposes a borrowed scalar value",
          "[node][state][scalar][generic][collection][feature:state]") {
    const auto state_node = node<generic_dict_state_value_node>();
    const auto input = ScalarValue<Dict<i64, i64>>{std::array{std::pair<i64, i64>{1, 7}}};
    const auto graph = [&] { return state_node(input); };

    CHECK(bg::run_graph(graph).equals<i64>({7}));
}

TEST_CASE("generic scalar state resolves constrained types and owns its value",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:generic]") {
    const auto state_node = node<generic_scalar_state_node>();

    const auto retain = [&](wp<TS<string>> value) { return state_node(value); };

    CHECK(bg::run_graph(retain, {"first", "second", "third"}).equals<string>({"first", "second", "third"}));
}

TEST_CASE("runtime-resolved scalar state reset releases its owned storage",
          "[node][state][scalar][generic][allocation][feature:state][scalar:generic]") {
    auto memory = counting_memory_resource{};
    {
        auto allocation = allocator{memory};
        auto state = GlobalState{allocation};
        const auto &runtime_type = cpp_scalar_runtime_type<string>();
        using Stored = details::scalar_stored_t<string>;
        auto input = std::optional{ScalarValue<string>{std::string(256, 'x'), allocation}};
        auto before = std::size_t{};
        auto after_set = std::size_t{};
        auto after_input_release = std::size_t{};
        auto after_reset = std::size_t{};

        state.write([&](auto &) {
            alignas(Stored) std::byte storage[sizeof(Stored)];
            runtime_type.construct_initial_value(storage);
            const auto cleanup = util::scope_exit{[&]() noexcept {
                auto allocation = details::current_scalar_allocator();
                runtime_type.destroy(storage, allocation);
            }};
            auto value = State<SCALAR>{runtime_type, storage};
            before = memory.allocations();
            value.set(input->view());
            after_set = memory.allocations();
            input.reset();
            after_input_release = memory.allocations();
            CHECK(scalar_cast<string>(value.value()).value() == std::string(256, 'x'));
            value.reset();
            after_reset = memory.allocations();
        });

        CHECK(after_set == before);
        CHECK(after_input_release == before);
        CHECK(after_reset < after_input_release);
    }

    CHECK(memory.allocations() == 0);
}

TEST_CASE("runtime-resolved scalar state rejects types without initial-state support",
          "[node][state][scalar][generic][feature:state][scalar:generic]") {
    const auto state_node = node<unsupported_scalar_state_node>();
    const auto input = ScalarValue<Opaque<std::string>>{std::string{"unsupported"}};
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        state_node(input);
    }

    wiring.finalize();
    CHECK_THROWS_AS(static_cast<void>(CompiledGraph{wiring}), std::invalid_argument);
}

namespace {

struct generic_set_dict_state_node {
    Scalar<Set<K>> set_type;
    Scalar<Dict<K, V>> dict_type;
    Scalar<K> key;
    Scalar<V> value;
    State<Set<K>> set_state;
    State<Dict<K, V>> dict_state;

    void start(EvaluationContext *context) const {
        if (!set_type.contains(key)) {
            throw std::logic_error{"generic set input does not contain its test key"};
        }

        (void)set_state->insert(key);
        (void)set_state->erase(key);
        (void)set_state->insert(key);
        dict_state->insert_or_assign(key, value);
        (void)dict_state->erase(key);
        dict_state->insert_or_assign(key, value);

        context->global_state().write([&](auto &global_state) {
            global_state.set(util::symbol{"dict_input"}, dict_type.at(key));
            global_state.set(util::symbol{"dict_result"}, dict_state->at(key));
            global_state.set(util::symbol{"set_result"},
                             ScalarValue{static_cast<i64>(set_state->size())});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic set and dictionary state use resolved storage",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:set][scalar:dict]"
          "[scalar:generic]") {
    const auto generic_set_dict_state = node<generic_set_dict_state_node>();

    const auto set_type = ScalarValue<Set<i64>>{std::array<i64, 1>{2}};
    const auto dict_type = ScalarValue<Dict<i64, string>>{
        std::array{std::pair{i64{2}, std::string_view{"one"}}}};
    const auto key = ScalarValue{i64{2}};
    const auto value = ScalarValue{"two"};
    auto state = GlobalState{};

    run_graph([&] { generic_set_dict_state(set_type, dict_type, key, value); }, GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) {
        CHECK(*scalar_cast<string>(values.at("dict_input")) == "one");
        CHECK(*scalar_cast<string>(values.at("dict_result")) == "two");
        CHECK(*scalar_cast<i64>(values.at("set_result")) == 1);
    });
}

namespace {

struct composed_dict_state_node {
    Scalar<K> key;
    Scalar<V> value;
    State<Dict<K, V>> state;
    Out<TS<V>> output;

    void start() const {
        state->insert_or_assign(key, value);
        output.set_value(state->at(key));
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic dictionary state composes storage from separate resolved types",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:dict]"
          "[scalar:generic]") {
    const auto composed_dict_state = node<composed_dict_state_node>();

    const auto key = ScalarValue{i64{2}};
    const auto value = ScalarValue{"two"};

    const auto graph = [&] { return composed_dict_state(key, value); };

    CHECK(bg::run_graph(graph).equals<string>({"two"}));
}

namespace {

struct composed_list_set_state_node {
    Scalar<K> value;
    State<List<K>> list;
    State<Set<K>> set;
    Out<TS<K>> output;

    void start() const {
        list->append(value);
        (void)set->insert(value);

        if (!set->contains(value)) {
            throw std::logic_error{"composed set does not contain its inserted value"};
        }

        output.set_value(list[0]);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic list and set state compose storage from their resolved item type",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:list][scalar:set]"
          "[scalar:generic]") {
    const auto composed_list_set_state = node<composed_list_set_state_node>();

    const auto value = ScalarValue{i64{3}};

    const auto graph = [&] { return composed_list_set_state(value); };

    CHECK(bg::run_graph(graph).equals<i64>({3}));
}

TEST_CASE("a runtime graph releases composed collection state storage",
          "[node][state][scalar][generic][declaration:generic][allocation][feature:state]") {
    const auto composed_dict_state = node<composed_dict_state_node>();

    const auto key = ScalarValue{i64{2}};
    const auto value = ScalarValue{"two"};
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        composed_dict_state(key, value);
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto state = GlobalState{};
    {
        auto runtime = RuntimeGraph{graph, allocator{memory}, state};
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

namespace {

struct generic_nested_state_node {
    Scalar<K> key;
    Scalar<List<V>> values;
    State<Dict<K, List<V>>> state;
    Out<TS<V>> output;

    void start() const {
        state->insert_or_assign(key, values);
        state->insert_or_assign(key, values);
        auto stored = state->at(key);
        stored->append(values[0]);

        output.set_value(stored[2]);
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic nested collection state returns mutable resolved items",
          "[node][state][scalar][generic][declaration:generic][collection][feature:state]"
          "[scalar:generic]") {
    const auto generic_nested_state = node<generic_nested_state_node>();

    const auto key = ScalarValue{i64{2}};
    const auto items = ScalarValue<List<string>>{std::array<std::string_view, 2>{"one", "two"}};

    const auto graph = [&] { return generic_nested_state(key, items); };

    CHECK(bg::run_graph(graph).equals<string>({"one"}));
}

namespace {

struct nested_collection_state_node {
    Scalar<List<i64>> initial;
    State<Dict<i64, List<i64>>> state;
    Out<TS<i64>> output;

    void start() const {
        state->insert_or_assign(1, initial);
        state->at(1).append(9);
        state->insert_or_assign(1, initial);

        output.set(static_cast<i64>(state->at(1).size()));
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("nested collection state returns mutable child views", "[node][state][scalar][collection][feature:state]") {
    const auto nested_collection_state = node<nested_collection_state_node>();

    const auto initial = ScalarValue<List<i64>>{std::array<i64, 1>{7}};

    const auto graph = [&] { return nested_collection_state(initial); };

    CHECK(bg::run_graph(graph).equals<i64>({1}));
}

TEST_CASE("runtime graphs have independent generic collection state",
          "[node][state][scalar][generic][declaration:generic][runtime][feature:state][scalar:generic]") {
    const auto generic_list_state = node<generic_list_state_node>();

    auto results = std::vector<i64>{};

    const auto observe = node([&](In<TS<i64>> input) {
        results.push_back(*input);
    });

    const auto input = ScalarValue<List<i64>>{std::array<i64, 2>{3, 4}};
    auto wiring = GraphWiring{};
    {
        const auto context = WiringContext{wiring};
        observe(generic_list_state(input));
    }

    wiring.finalize();
    const auto graph = CompiledGraph{wiring};
    for (auto run = 0; run < 2; ++run) {
        auto state = GlobalState{};
        auto runtime = RuntimeGraph{graph, state};
        SimulationEvaluationEngine{}.run(runtime, MIN_DT + std::chrono::microseconds{1});
        CHECK(results.size() == static_cast<std::size_t>(run + 1));
        CHECK(results.back() == 4);
    }
}

static_assert(std::derived_from<MutableList<i64>, List<i64>>);
static_assert(std::derived_from<MutableSet<i64>, Set<i64>>);
static_assert(std::derived_from<MutableDict<i64, i64>, Dict<i64, i64>>);
static_assert(sizeof(State<Dict<SCALAR, List<SCALAR>>>) == 2 * sizeof(void *));

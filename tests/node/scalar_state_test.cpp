#include <bg/bg.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace {

struct collection_state_node {
    bg::State<bg::List<bg::i64>> list;
    bg::State<bg::Set<bg::i64>> set;
    bg::State<bg::Dict<bg::i64, bg::i64>> dict;

    void start(bg::EvaluationContext *context) const {
        list->append(1);
        list->append(3);
        list->set(1, 2);

        (void)set->insert(4);
        (void)set->insert(4);
        (void)set->insert(5);

        dict->insert_or_assign(6, 7);
        dict->insert_or_assign(6, 8);

        const auto result = bg::ScalarValue{static_cast<bg::i64>(list->size() + set->size() + dict->at(6))};
        context->global_state().write([&](auto &state) { state.set(bg::util::symbol{"result"}, result); });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("collection state is mutable during a node invocation", "[node][state][scalar][feature:state]") {
    const auto collection_state = bg::node<collection_state_node>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { collection_state(); }, bg::GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) { CHECK(*bg::scalar_cast<bg::i64>(values.at("result")) == 12); });
}

namespace {

struct collection_copy_node {
    bg::Scalar<bg::List<bg::string>> list;
    bg::Scalar<bg::Set<bg::string>> set;
    bg::Scalar<bg::Dict<bg::string, bg::string>> dict;
    bg::Scalar<bg::Dict<bg::string, bg::List<bg::string>>> nested_dict;
    bg::Scalar<bg::Opaque<const char *>> list_item_data;
    bg::Scalar<bg::Opaque<const char *>> set_item_data;
    bg::Scalar<bg::Opaque<const char *>> dict_key_data;
    bg::Scalar<bg::Opaque<const char *>> dict_value_data;
    bg::Scalar<bg::Opaque<const char *>> nested_item_data;

    void start(bg::EvaluationContext *context) const {
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
            values.set(bg::util::symbol{"list_item_matches"},
                       bg::ScalarValue{list[0].value().data() == list_item_data.value()});
            values.set(bg::util::symbol{"set_item_matches"}, bg::ScalarValue{set_item_matches});
            values.set(bg::util::symbol{"dict_key_matches"}, bg::ScalarValue{dict_key_matches});
            values.set(bg::util::symbol{"dict_value_matches"}, bg::ScalarValue{dict_value_matches});
            values.set(bg::util::symbol{"nested_item_matches"}, bg::ScalarValue{nested_item_matches});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("compiled graph collection copies retain their string items",
          "[node][scalar][scalar:list][scalar:set][scalar:dict][scalar:string]"
          "[feature:scalar-storage][feature:graph-compilation][feature:global-state][storage:variable]") {
    const auto collection_copy = bg::node<collection_copy_node>();
    const auto list = bg::ScalarValue<bg::List<bg::string>>{std::array<std::string_view, 1>{"list item"}};
    const auto set = bg::ScalarValue<bg::Set<bg::string>>{std::array<std::string_view, 1>{"set item"}};
    const auto dict = bg::ScalarValue<bg::Dict<bg::string, bg::string>>{
        std::array{std::pair{std::string_view{"dict key"}, std::string_view{"dict value"}}}};
    const auto nested_items = bg::ScalarValue<bg::List<bg::string>>{
        std::array<std::string_view, 1>{"nested item"}};
    const auto nested_dict = bg::ScalarValue<bg::Dict<bg::string, bg::List<bg::string>>>{
        std::array{std::pair{std::string_view{"nested"}, nested_items.view()}}};
    const auto list_item_data = bg::ScalarValue<bg::Opaque<const char *>>{list.view()[0].value().data()};
    auto set_item_text = static_cast<const char *>(nullptr);
    set.view().for_each([&](const auto item) { set_item_text = item.value().data(); });
    auto dict_key_text = static_cast<const char *>(nullptr);
    auto dict_value_text = static_cast<const char *>(nullptr);
    dict.view().for_each([&](const auto key, const auto value) {
        dict_key_text = key.value().data();
        dict_value_text = value.value().data();
    });
    const auto nested_item_text = nested_dict.view().at("nested")[0].value().data();
    const auto set_item_data = bg::ScalarValue<bg::Opaque<const char *>>{set_item_text};
    const auto dict_key_data = bg::ScalarValue<bg::Opaque<const char *>>{dict_key_text};
    const auto dict_value_data = bg::ScalarValue<bg::Opaque<const char *>>{dict_value_text};
    const auto nested_item_data = bg::ScalarValue<bg::Opaque<const char *>>{nested_item_text};
    auto global_state = bg::GlobalState{};

    bg::run_graph(
        [&] {
            collection_copy(list, set, dict, nested_dict, list_item_data, set_item_data, dict_key_data,
                            dict_value_data, nested_item_data);
        },
        bg::GraphRunOptions{.global_state = &global_state});

    global_state.read([](const auto &values) {
        CHECK(*bg::scalar_cast<bool>(values.at("list_item_matches")));
        CHECK(*bg::scalar_cast<bool>(values.at("set_item_matches")));
        CHECK(*bg::scalar_cast<bool>(values.at("dict_key_matches")));
        CHECK(*bg::scalar_cast<bool>(values.at("dict_value_matches")));
        CHECK(*bg::scalar_cast<bool>(values.at("nested_item_matches")));
    });
}

TEST_CASE("a runtime graph releases collection state storage", "[node][state][scalar][allocation][feature:state]") {
    const auto collection_state = bg::node<collection_state_node>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        collection_state();
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = bg::GlobalState{};
    {
        auto runtime = bg::RuntimeGraph{graph, bg::allocator{memory}, global_state};
        bg::SimulationEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

namespace {

struct generic_list_state_node {
    bg::Scalar<bg::List<bg::SCALAR>> values;
    bg::State<bg::List<bg::SCALAR>> state;

    void start(bg::EvaluationContext *context) const {
        state->append(values[0]);
        state->append(values[1]);
        state->set(0, values[1]);
        state->erase(1);

        context->global_state().write(
            [&](auto &global_state) { global_state.set(bg::util::symbol{"generic_result"}, state[0]); });
    }

    void evaluate() const {}
};

using K = bg::ScalarVariable<"K", bg::Hashable>;
using V = bg::ScalarVariable<"V">;

} // namespace

TEST_CASE("generic collection state uses its resolved storage",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:generic]") {
    const auto generic_list_state = bg::node<generic_list_state_node>();
    const auto input = bg::ScalarValue<bg::List<bg::i64>>{std::array<bg::i64, 2>{3, 4}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_list_state(input); }, bg::GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) { CHECK(*bg::scalar_cast<bg::i64>(values.at("generic_result")) == 4); });
}

namespace {

struct generic_set_dict_state_node {
    bg::Scalar<bg::Set<K>> set_type;
    bg::Scalar<bg::Dict<K, V>> dict_type;
    bg::Scalar<K> key;
    bg::Scalar<V> value;
    bg::State<bg::Set<K>> set_state;
    bg::State<bg::Dict<K, V>> dict_state;

    void start(bg::EvaluationContext *context) const {
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
            global_state.set(bg::util::symbol{"dict_input"}, dict_type.at(key));
            global_state.set(bg::util::symbol{"dict_result"}, dict_state->at(key));
            global_state.set(bg::util::symbol{"set_result"},
                             bg::ScalarValue{static_cast<bg::i64>(set_state->size())});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic set and dictionary state use resolved storage",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:set][scalar:dict]"
          "[scalar:generic]") {
    const auto generic_set_dict_state = bg::node<generic_set_dict_state_node>();
    const auto set_type = bg::ScalarValue<bg::Set<bg::i64>>{std::array<bg::i64, 1>{2}};
    const auto dict_type = bg::ScalarValue<bg::Dict<bg::i64, bg::string>>{
        std::array{std::pair{bg::i64{2}, std::string_view{"one"}}}};
    const auto key = bg::ScalarValue{bg::i64{2}};
    const auto value = bg::ScalarValue{"two"};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_set_dict_state(set_type, dict_type, key, value); }, bg::GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) {
        CHECK(*bg::scalar_cast<bg::string>(values.at("dict_input")) == "one");
        CHECK(*bg::scalar_cast<bg::string>(values.at("dict_result")) == "two");
        CHECK(*bg::scalar_cast<bg::i64>(values.at("set_result")) == 1);
    });
}

namespace {

struct composed_dict_state_node {
    bg::Scalar<K> key;
    bg::Scalar<V> value;
    bg::State<bg::Dict<K, V>> state;

    void start(bg::EvaluationContext *context) const {
        state->insert_or_assign(key, value);

        context->global_state().write(
            [&](auto &global_state) { global_state.set(bg::util::symbol{"composed_dict_result"}, state->at(key)); });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic dictionary state composes storage from separate resolved types",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:dict]"
          "[scalar:generic]") {
    const auto composed_dict_state = bg::node<composed_dict_state_node>();
    const auto key = bg::ScalarValue{bg::i64{2}};
    const auto value = bg::ScalarValue{"two"};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { composed_dict_state(key, value); }, bg::GraphRunOptions{.global_state = &state});

    state.read(
        [](const auto &values) { CHECK(*bg::scalar_cast<bg::string>(values.at("composed_dict_result")) == "two"); });
}

namespace {

struct composed_list_set_state_node {
    bg::Scalar<K> value;
    bg::State<bg::List<K>> list;
    bg::State<bg::Set<K>> set;

    void start(bg::EvaluationContext *context) const {
        list->append(value);
        (void)set->insert(value);

        if (!set->contains(value)) {
            throw std::logic_error{"composed set does not contain its inserted value"};
        }

        context->global_state().write(
            [&](auto &global_state) { global_state.set(bg::util::symbol{"composed_list_result"}, list[0]); });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic list and set state compose storage from their resolved item type",
          "[node][state][scalar][generic][declaration:generic][feature:state][scalar:list][scalar:set]"
          "[scalar:generic]") {
    const auto composed_list_set_state = bg::node<composed_list_set_state_node>();
    const auto value = bg::ScalarValue{bg::i64{3}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { composed_list_set_state(value); }, bg::GraphRunOptions{.global_state = &state});

    state.read(
        [](const auto &values) { CHECK(*bg::scalar_cast<bg::i64>(values.at("composed_list_result")) == 3); });
}

TEST_CASE("a runtime graph releases composed collection state storage",
          "[node][state][scalar][generic][declaration:generic][allocation][feature:state]") {
    const auto composed_dict_state = bg::node<composed_dict_state_node>();
    const auto key = bg::ScalarValue{bg::i64{2}};
    const auto value = bg::ScalarValue{"two"};
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        composed_dict_state(key, value);
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto state = bg::GlobalState{};
    {
        auto runtime = bg::RuntimeGraph{graph, bg::allocator{memory}, state};
        bg::SimulationEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{1});
    }

    CHECK(memory.allocations() == 0);
}

namespace {

struct generic_nested_state_node {
    bg::Scalar<K> key;
    bg::Scalar<bg::List<V>> values;
    bg::State<bg::Dict<K, bg::List<V>>> state;

    void start(bg::EvaluationContext *context) const {
        state->insert_or_assign(key, values);
        state->insert_or_assign(key, values);
        auto stored = state->at(key);
        stored->append(values[0]);

        context->global_state().write(
            [&](auto &global_state) { global_state.set(bg::util::symbol{"generic_nested_result"}, stored[2]); });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic nested collection state returns mutable resolved items",
          "[node][state][scalar][generic][declaration:generic][collection][feature:state]"
          "[scalar:generic]") {
    const auto generic_nested_state = bg::node<generic_nested_state_node>();
    const auto key = bg::ScalarValue{bg::i64{2}};
    const auto items = bg::ScalarValue<bg::List<bg::string>>{std::array<std::string_view, 2>{"one", "two"}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_nested_state(key, items); }, bg::GraphRunOptions{.global_state = &state});

    state.read(
        [](const auto &values) { CHECK(*bg::scalar_cast<bg::string>(values.at("generic_nested_result")) == "one"); });
}

namespace {

struct nested_collection_state_node {
    bg::Scalar<bg::List<bg::i64>> initial;
    bg::State<bg::Dict<bg::i64, bg::List<bg::i64>>> state;

    void start(bg::EvaluationContext *context) const {
        state->insert_or_assign(1, initial);
        state->at(1).append(9);
        state->insert_or_assign(1, initial);

        context->global_state().write([&](auto &global_state) {
            global_state.set(bg::util::symbol{"nested_result"},
                             bg::ScalarValue{static_cast<bg::i64>(state->at(1).size())});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("nested collection state returns mutable child views", "[node][state][scalar][collection][feature:state]") {
    const auto nested_collection_state = bg::node<nested_collection_state_node>();
    const auto initial = bg::ScalarValue<bg::List<bg::i64>>{std::array<bg::i64, 1>{7}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { nested_collection_state(initial); }, bg::GraphRunOptions{.global_state = &state});

    state.read([](const auto &values) { CHECK(*bg::scalar_cast<bg::i64>(values.at("nested_result")) == 1); });
}

TEST_CASE("runtime graphs have independent generic collection state",
          "[node][state][scalar][generic][declaration:generic][runtime][feature:state][scalar:generic]") {
    const auto generic_list_state = bg::node<generic_list_state_node>();
    const auto input = bg::ScalarValue<bg::List<bg::i64>>{std::array<bg::i64, 2>{3, 4}};
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        generic_list_state(input);
    }

    wiring.finalize();
    const auto graph = bg::CompiledGraph{wiring};
    for (auto run = 0; run < 2; ++run) {
        auto state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{graph, state};
        bg::SimulationEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{1});

        state.read([](const auto &values) { CHECK(*bg::scalar_cast<bg::i64>(values.at("generic_result")) == 4); });
    }
}

static_assert(std::derived_from<bg::MutableList<bg::i64>, bg::List<bg::i64>>);
static_assert(std::derived_from<bg::MutableSet<bg::i64>, bg::Set<bg::i64>>);
static_assert(std::derived_from<bg::MutableDict<bg::i64, bg::i64>, bg::Dict<bg::i64, bg::i64>>);
static_assert(sizeof(bg::State<bg::Dict<bg::SCALAR, bg::List<bg::SCALAR>>>) == 2 * sizeof(void *));

#include <bg/bg.hpp>

#include "../utils/counting_memory_resource.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace {

struct collection_state_node {
    bg::State<bg::List<std::int64_t>> list;
    bg::State<bg::Set<std::int64_t>> set;
    bg::State<bg::Dict<std::int64_t, std::int64_t>> dict;

    void start(bg::EvaluationContext *context) const {
        list->append(1);
        list->append(3);
        list->set(1, 2);

        (void)set->insert(4);
        (void)set->insert(4);
        (void)set->insert(5);

        dict->insert_or_assign(6, 7);
        dict->insert_or_assign(6, 8);

        const auto result = bg::ScalarValue{static_cast<std::int64_t>(list->size() + set->size() + dict->at(6))};
        context->global_state().write([&](auto &state) { state.set(bg::util::symbol{"result"}, result); });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("collection state is mutable during a node invocation", "[node][state][scalar]") {
    const auto collection_state = bg::node<collection_state_node>();
    auto state = bg::GlobalState{};

    bg::run_graph([&] { collection_state(); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<std::int64_t>(values.at("result"))->value() == 12);
    });
}

TEST_CASE("a runtime graph releases collection state storage", "[node][state][scalar][allocation]") {
    const auto collection_state = bg::node<collection_state_node>();
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        collection_state();
    }

    const auto graph = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto global_state = bg::GlobalState{};
    {
        auto runtime = bg::RuntimeGraph{graph, memory, global_state};
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

TEST_CASE("generic collection state uses its resolved storage", "[node][state][scalar][generic]") {
    const auto generic_list_state = bg::node<generic_list_state_node>();
    const auto input = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 2>{3, 4}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_list_state(input); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<std::int64_t>(values.at("generic_result"))->value() == 4);
    });
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
                             bg::ScalarValue{static_cast<std::int64_t>(set_state->size())});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic set and dictionary state use resolved storage", "[node][state][scalar][generic]") {
    const auto generic_set_dict_state = bg::node<generic_set_dict_state_node>();
    const auto set_type = bg::ScalarValue<bg::Set<std::int64_t>>{std::array<std::int64_t, 1>{2}};
    const auto dict_type = bg::ScalarValue<bg::Dict<std::int64_t, bg::string>>{
        std::array{std::pair{std::int64_t{2}, std::string_view{"one"}}}};
    const auto key = bg::ScalarValue{std::int64_t{2}};
    const auto value = bg::ScalarValue{"two"};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_set_dict_state(set_type, dict_type, key, value); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<bg::string>(values.at("dict_input"))->value() == "one");
        CHECK(bg::scalar_cast<bg::string>(values.at("dict_result"))->value() == "two");
        CHECK(bg::scalar_cast<std::int64_t>(values.at("set_result"))->value() == 1);
    });
}

namespace {

struct composed_dict_state_node {
    bg::Scalar<K> key;
    bg::Scalar<V> value;
    bg::State<bg::Dict<K, V>> state;

    void start(bg::EvaluationContext *context) const {
        state->insert_or_assign(key, value);

        context->global_state().write([&](auto &global_state) {
            global_state.set(bg::util::symbol{"composed_dict_result"}, state->at(key));
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic dictionary state composes storage from separate resolved types", "[node][state][scalar][generic]") {
    const auto composed_dict_state = bg::node<composed_dict_state_node>();
    const auto key = bg::ScalarValue{std::int64_t{2}};
    const auto value = bg::ScalarValue{"two"};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { composed_dict_state(key, value); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<bg::string>(values.at("composed_dict_result"))->value() == "two");
    });
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

        context->global_state().write([&](auto &global_state) {
            global_state.set(bg::util::symbol{"composed_list_result"}, list[0]);
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic list and set state compose storage from their resolved item type",
          "[node][state][scalar][generic]") {
    const auto composed_list_set_state = bg::node<composed_list_set_state_node>();
    const auto value = bg::ScalarValue{std::int64_t{3}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { composed_list_set_state(value); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<std::int64_t>(values.at("composed_list_result"))->value() == 3);
    });
}

TEST_CASE("a runtime graph releases composed collection state storage", "[node][state][scalar][generic][allocation]") {
    const auto composed_dict_state = bg::node<composed_dict_state_node>();
    const auto key = bg::ScalarValue{std::int64_t{2}};
    const auto value = bg::ScalarValue{"two"};
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        composed_dict_state(key, value);
    }

    const auto graph = bg::CompiledGraph{wiring};
    auto memory = counting_memory_resource{};
    auto state = bg::GlobalState{};
    {
        auto runtime = bg::RuntimeGraph{graph, memory, state};
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

        context->global_state().write([&](auto &global_state) {
            global_state.set(bg::util::symbol{"generic_nested_result"}, stored[2]);
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("generic nested collection state returns mutable resolved items",
          "[node][state][scalar][generic][collection]") {
    const auto generic_nested_state = bg::node<generic_nested_state_node>();
    const auto key = bg::ScalarValue{std::int64_t{2}};
    const auto items = bg::ScalarValue<bg::List<bg::string>>{std::array<std::string_view, 2>{"one", "two"}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { generic_nested_state(key, items); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<bg::string>(values.at("generic_nested_result"))->value() == "one");
    });
}

namespace {

struct nested_collection_state_node {
    bg::Scalar<bg::List<std::int64_t>> initial;
    bg::State<bg::Dict<std::int64_t, bg::List<std::int64_t>>> state;

    void start(bg::EvaluationContext *context) const {
        state->insert_or_assign(1, initial);
        state->at(1).append(9);
        state->insert_or_assign(1, initial);

        context->global_state().write([&](auto &global_state) {
            global_state.set(bg::util::symbol{"nested_result"},
                             bg::ScalarValue{static_cast<std::int64_t>(state->at(1).size())});
        });
    }

    void evaluate() const {}
};

} // namespace

TEST_CASE("nested collection state returns mutable child views", "[node][state][scalar][collection]") {
    const auto nested_collection_state = bg::node<nested_collection_state_node>();
    const auto initial = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 1>{7}};
    auto state = bg::GlobalState{};

    bg::run_graph([&] { nested_collection_state(initial); }, state);

    state.read([](const auto &values) {
        CHECK(bg::scalar_cast<std::int64_t>(values.at("nested_result"))->value() == 1);
    });
}

TEST_CASE("runtime graphs have independent generic collection state", "[node][state][scalar][generic][runtime]") {
    const auto generic_list_state = bg::node<generic_list_state_node>();
    const auto input = bg::ScalarValue<bg::List<std::int64_t>>{std::array<std::int64_t, 2>{3, 4}};
    auto wiring = bg::GraphWiring{};
    {
        const auto context = bg::WiringContext{wiring};
        generic_list_state(input);
    }

    const auto graph = bg::CompiledGraph{wiring};
    for (auto run = 0; run < 2; ++run) {
        auto state = bg::GlobalState{};
        auto runtime = bg::RuntimeGraph{graph, state};
        bg::SimulationEvaluationEngine{}.run(runtime, bg::MIN_DT + std::chrono::microseconds{1});

        state.read([](const auto &values) {
            CHECK(bg::scalar_cast<std::int64_t>(values.at("generic_result"))->value() == 4);
        });
    }
}

static_assert(std::derived_from<bg::MutableList<std::int64_t>, bg::List<std::int64_t>>);
static_assert(std::derived_from<bg::MutableSet<std::int64_t>, bg::Set<std::int64_t>>);
static_assert(std::derived_from<bg::MutableDict<std::int64_t, std::int64_t>, bg::Dict<std::int64_t, std::int64_t>>);
static_assert(sizeof(bg::State<bg::Dict<bg::SCALAR, bg::List<bg::SCALAR>>>) == 2 * sizeof(void *));

#include <bg/literal/scalar_literal.hpp>

#include "literal_input/time_series_apply.hpp"

#include "test_scalar_provider.hpp"

#include <bg/node.hpp>
#include <bg/scalar/bundle/details/allocation_owner.hpp>
#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/set/scalar.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/testing/test_graph.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <source_location>
#include <string>
#include <memory_resource>
#include <optional>
#include <vector>

namespace {

using bg::In;
using bg::Out;
using bg::TS;
using bg::TSL;
using bg::i64;

template <template <typename, auto...> typename Field> struct ProbeBundle {
    Field<TS<i64>> item;
};

struct alignas(64) LiteralCompound {
    i64 first{};
    const i64 second{};
};

template <template <typename, auto...> typename Field> struct LiteralBundle {
    Field<bg::Dict<bg::string, bg::List<i64>>> nested;
    Field<LiteralCompound> compound;
};

void require(bool condition, std::source_location location = std::source_location::current()) {
    if (!condition) {
        throw std::runtime_error{"time-series literal probe check failed at line " + std::to_string(location.line())};
    }
}

struct ListObservation {
    std::array<bool, 3> updated;
    std::array<i64, 3> values;
    bool valid;
};

struct RuntimeListItem {
    i64 first;
    i64 second;

    RuntimeListItem(i64 first, i64 second) : first{first}, second{second} {}
    RuntimeListItem(const RuntimeListItem &other) : first{other.first}, second{other.second} { ++copy_count; }
    RuntimeListItem(RuntimeListItem &&) noexcept = default;

    inline static std::size_t copy_count{};
};

class AllocationCounter final : public std::pmr::memory_resource {
  public:
    std::size_t allocations() const noexcept { return allocations_; }
    std::size_t outstanding_allocations() const noexcept { return outstanding_allocations_; }

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto *result = upstream_->allocate(bytes, alignment);
        ++allocations_;
        ++outstanding_allocations_;
        return result;
    }

    void do_deallocate(void *memory, std::size_t bytes, std::size_t alignment) override {
        upstream_->deallocate(memory, bytes, alignment);
        --outstanding_allocations_;
    }

    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override { return this == &other; }

    std::pmr::memory_resource *upstream_{std::pmr::new_delete_resource()};
    std::size_t allocations_{};
    std::size_t outstanding_allocations_{};
};

std::vector<ListObservation> list_observations;
std::vector<bool> root_validity;
bool duplicate_prevalidation_preserved_output{};
bool partial_update_observed{};

void verify_scalar_leaf_and_deduplicated_output() {
    const auto set = bg::node([](In<TS<i64>> input, Out<TS<i64, bg::TimeSeriesGuarantee::deduplicated>> output) {
        literal::apply(output, *input);
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return set(input); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 3>{4, 4, 7});

    require(actual.equals<i64>({4, std::nullopt, 7}));
}

void verify_fixed_list_delta_and_notifications() {
    list_observations.clear();
    const auto patch = bg::node([](In<TS<i64>> command,
                                  Out<TSL<TS<i64, bg::TimeSeriesGuarantee::deduplicated>, 3>> output) {
        if (*command == 1) {
            literal::apply(output, {{0, 10}, {2, 30}});
            return;
        }
        if (*command == 2) {
            literal::apply(output, {{2, 31}});
            return;
        }
        literal::apply(output, {{2, 31}});
    });
    const auto observe = bg::node([](In<TSL<TS<i64, bg::TimeSeriesGuarantee::deduplicated>, 3>> input,
                                    Out<TS<i64>> output) {
        auto observation = ListObservation{.updated = {}, .values = {}, .valid = input.valid()};
        for (auto index = std::size_t{}; index < observation.updated.size(); ++index) {
            observation.updated[index] = input.item_updated(index);
            if (input[index].valid()) {
                observation.values[index] = *input[index];
            }
        }
        list_observations.push_back(observation);
        output.set(observation.values[0] * 100 + observation.values[2]);
    });
    const auto graph = [&](bg::wp<TS<i64>> command) { return observe(patch(command)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 3>{1, 2, 3});

    require(actual.equals<i64>({1030, 1031, std::nullopt}));
    require(list_observations.size() == 2);
    require(list_observations[0].valid);
    require((list_observations[0].updated == std::array{true, false, true}));
    require((list_observations[0].values == std::array<i64, 3>{10, 0, 30}));
    require((list_observations[1].updated == std::array{false, false, true}));
    require((list_observations[1].values == std::array<i64, 3>{10, 0, 31}));
}

void verify_nested_list_delta() {
    const auto patch = bg::node([](In<TS<i64>> input, Out<TSL<TSL<TS<i64>, 2>, 2>> output) {
        literal::apply(output, {{1, {{0, *input}}}});
    });
    const auto read = bg::node([](In<TSL<TSL<TS<i64>, 2>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[1][0]);
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return read(patch(input)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 2>{8, 9});

    require(actual.equals<i64>({8, 9}));
}

void verify_runtime_output_dispatch() {
    const auto seed = bg::node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        literal::apply(output, {{1, *input}});
    });
    const auto patch = bg::node([](In<bg::TS_T> source, In<TS<i64>> command, Out<bg::TS_T> output) {
        if (source.valid()) {
            literal::apply(output, {{0, *command}, {1, *command}});
        }
    });
    const auto read = bg::node([](In<TSL<TS<i64>, 2>> input, Out<TS<i64>> output) {
        output.set(*input[0] + *input[1]);
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return read(patch(seed(input), input)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 2>{5, 6});

    require(actual.equals<i64>({10, 12}));
}

void verify_empty_root_and_nested_deltas() {
    root_validity.clear();
    const auto patch = bg::node([](In<TS<i64>> command, Out<TSL<TS<i64>, 2>> output) {
        if (*command == 2) {
            literal::apply(output, {{0, 5}});
            return;
        }
        literal::apply(output, literal::ScalarLiteral{});
    });
    const auto observe = bg::node([](In<TS<i64>> command, In<TSL<TS<i64>, 2>, bg::optional> input,
                                     Out<TS<i64>> output) {
        root_validity.push_back(input.valid());
        output.set((input.valid() ? 100 : 0) + (input[0].valid() ? *input[0] : 0));
        static_cast<void>(command);
    });
    const auto graph = [&](bg::wp<TS<i64>> command) { return observe(command, patch(command)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 3>{1, 2, 3});

    require(actual.equals<i64>({0, 105, 105}));
    require((root_validity == std::vector<bool>{false, true, true}));

    const auto nested_patch = bg::node([](In<TS<i64>> command, Out<TSL<TSL<TS<i64>, 2>, 2>> output) {
        if (*command == 8) {
            literal::apply(output, {{1, {{0, *command}}}});
            return;
        }
        literal::apply(output, {{1, {}}});
    });
    const auto nested_observe = bg::node([](In<TS<i64>> command,
                                            In<TSL<TSL<TS<i64>, 2>, 2>, bg::optional> input,
                                            Out<TS<i64>> output) {
        const auto inner_valid = input[1].valid();
        const auto leaf_valid = input[1][0].valid();
        const auto value = leaf_valid ? *input[1][0] : 0;
        output.set((input.valid() ? 100 : 0) + (inner_valid ? 10 : 0) + value);
        static_cast<void>(command);
    });
    const auto nested_graph = [&](bg::wp<TS<i64>> command) { return nested_observe(command, nested_patch(command)); };
    const auto nested_actual = bg::testing::test_graph(nested_graph, std::array<i64, 3>{1, 8, 3});

    require(nested_actual.equals<i64>({0, 118, 118}));
}

void verify_equal_deduplicated_value_revalidates() {
    const auto update = bg::node([](In<TS<i64>> command, Out<TS<i64, bg::TimeSeriesGuarantee::deduplicated>> output) {
        if (*command == 2) {
            output.invalidate();
            return;
        }
        literal::apply(output, 5);
    });
    const auto graph = [&](bg::wp<TS<i64>> command) { return update(command); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 3>{1, 2, 3});

    require(actual.equals<i64>({5, std::nullopt, 5}));

    const auto list_update = bg::node([](In<TS<i64>> command,
                                         Out<TS<bg::List<i64>, bg::TimeSeriesGuarantee::deduplicated>> output) {
        if (*command == 2) {
            output.invalidate();
        }
        if (*command == 4) {
            output.apply({});
            return;
        }
        output.apply({2, 3});
    });
    const auto list_read = bg::node([](In<TS<bg::List<i64>, bg::TimeSeriesGuarantee::deduplicated>, bg::optional> input,
                                       Out<TS<i64>> output) {
        if (!input.valid()) {
            output.set(-1);
            return;
        }
        const auto values = input.value_view();
        if (values.size() == 0) {
            output.set(0);
            return;
        }
        output.set(values[0].value() + values[1].value());
    });
    const auto list_graph = [&](bg::wp<TS<i64>> command) { return list_read(list_update(command)); };
    const auto list_actual = bg::testing::test_graph(list_graph, std::array<i64, 4>{1, 2, 3, 4});

    require(list_actual.equals<i64>({5, 5, std::nullopt, 0}));
}

void verify_scalar_list_output() {
    const auto seed = bg::node([](In<TS<i64>> command, Out<TS<bg::List<i64>>> output) {
        output.apply({0, 1});
        static_cast<void>(command);
    });
    const auto patch = bg::node([](In<bg::TS_T> source, In<TS<i64>> command, Out<bg::TS_T> output) {
        if (source.valid()) {
            output.apply({*command, 2});
        }
    });
    const auto read_runtime = bg::node([](In<TS<bg::List<i64>>> input, Out<TS<i64>> output) {
        const auto values = input.value_view();
        output.set(values[0].value() + values[1].value());
    });
    const auto runtime_graph = [&](bg::wp<TS<i64>> command) { return read_runtime(patch(seed(command), command)); };
    const auto runtime_actual = bg::testing::test_graph(runtime_graph, std::array<i64, 2>{3, 4});
    require(runtime_actual.equals<i64>({5, 6}));

    const auto nested_patch = bg::node([](In<TS<i64>> command, Out<TSL<TS<bg::List<i64>>, 2>> output) {
        literal::apply(output, {{1, {*command, 2}}});
    });
    const auto read_nested = bg::node([](In<TSL<TS<bg::List<i64>>, 2>> input, Out<TS<i64>> output) {
        const auto values = input[1].value_view();
        output.set(values[0].value() + values[1].value());
    });
    const auto nested_graph = [&](bg::wp<TS<i64>> command) { return read_nested(nested_patch(command)); };
    const auto nested_actual = bg::testing::test_graph(nested_graph, std::array<i64, 2>{5, 6});
    require(nested_actual.equals<i64>({7, 8}));
}

template <typename ScalarRuntime, typename Verify>
void verify_direct_scalar_output(const ScalarRuntime &scalar_runtime, const bg::ScalarType &scalar_type,
                                 const literal::ScalarLiteral &literal,
                                 std::optional<std::size_t> expected_allocations, Verify &&verify,
                                 bool verify_failed_replacement = false) {
    const auto &time_series_type = bg::time_series_value_type(scalar_type, bg::TimeSeriesGuarantee::ordinary);
    const auto &time_series_runtime = bg::time_series_value_runtime_type(time_series_type, scalar_runtime);
    auto scalar_memory = AllocationCounter{};
    auto scalar_allocator = bg::allocator{scalar_memory};
    auto owner_resource = std::pmr::unsynchronized_pool_resource{};
    auto owner_allocator = bg::allocator{owner_resource};
    auto *allocation_owner = bg::details::BundleAllocationOwner::create(owner_allocator);
    auto release_allocation_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
    auto output_storage_resource = std::pmr::unsynchronized_pool_resource{};
    auto output_allocator = bg::allocator{output_storage_resource};
    auto *storage = output_allocator.allocate_bytes(scalar_runtime.size(), scalar_runtime.alignment());
    scalar_runtime.construct_initial_value(storage);

    auto time_series_state = bg::TimeSeriesState{};
    const auto evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{1}};
    auto output_state = bg::details::OutputState{time_series_runtime};
    output_state.bind(time_series_state, storage, evaluation_time);
    const auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};

    {
        auto context = bg::details::ScalarAllocationContext{scalar_allocator, *allocation_owner, nullptr, 0};
        auto release_storage = bg::util::scope_exit{[&]() noexcept {
            scalar_runtime.destroy(storage, scalar_allocator);
            output_allocator.deallocate_bytes(storage, scalar_runtime.size(), scalar_runtime.alignment());
        }};
        const auto allocations_before = scalar_memory.allocations();
        output.apply(literal);
        if (expected_allocations) {
            require(scalar_memory.allocations() - allocations_before == *expected_allocations);
        }
        require(output.valid());
        verify(scalar_runtime, storage);

        if (scalar_type.supports_equality()) {
            auto construction_context = bg::details::ScalarLiteralConstructionContext{literal, true};
            const auto construction = bg::ScalarConstruction{
                &construction_context, &bg::details::ScalarLiteralConstructionContext::callback};
            const auto outstanding_before_assignment = scalar_memory.outstanding_allocations();
            require(!scalar_runtime.assign_with(storage, construction, true));
            require(scalar_memory.outstanding_allocations() == outstanding_before_assignment);
            verify(scalar_runtime, storage);
        }

        if (verify_failed_replacement) {
            const auto next_evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{2}};
            output_state.bind(time_series_state, storage, next_evaluation_time);
            const auto valid_before_failure = output.valid();
            const auto time_before_failure = output.last_updated();
            const auto state_time_before_failure = time_series_state.time();
            const auto allocations_before_failure = scalar_memory.allocations();
            const auto outstanding_before_failure = scalar_memory.outstanding_allocations();
            auto rejected = false;
            try {
                output.apply({3, "bad"});
            } catch (const std::invalid_argument &) {
                rejected = true;
            }

            require(rejected);
            require(output.valid() == valid_before_failure);
            require(output.last_updated() == time_before_failure);
            require(time_series_state.time() == state_time_before_failure);
            require(scalar_memory.allocations() > allocations_before_failure);
            require(scalar_memory.outstanding_allocations() == outstanding_before_failure);
            verify(scalar_runtime, storage);
        }
    }

    release_allocation_owner.release();
    allocation_owner->release();
}

template <typename Verify>
void verify_retained_scalar_snapshot(const bg::ScalarRuntimeType &scalar_runtime, const bg::ScalarType &scalar_type,
                                   const literal::ScalarLiteral &initial, const literal::ScalarLiteral &replacement,
                                   const literal::ScalarLiteral &invalid_replacement, Verify &&verify) {
    const auto &time_series_type = bg::time_series_value_type(scalar_type, bg::TimeSeriesGuarantee::ordinary);
    const auto &time_series_runtime = bg::time_series_value_runtime_type(time_series_type, scalar_runtime);
    auto scalar_memory = AllocationCounter{};
    auto scalar_allocator = bg::allocator{scalar_memory};
    auto owner_resource = std::pmr::unsynchronized_pool_resource{};
    auto owner_allocator = bg::allocator{owner_resource};
    auto *allocation_owner = bg::details::BundleAllocationOwner::create(owner_allocator);
    auto release_allocation_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
    auto snapshot_resource = std::pmr::unsynchronized_pool_resource{};
    auto snapshot_allocator = bg::allocator{snapshot_resource};
    auto snapshot = std::optional<bg::ScalarValue<>>{};
    auto output_storage_resource = std::pmr::unsynchronized_pool_resource{};
    auto output_allocator = bg::allocator{output_storage_resource};
    auto *storage = output_allocator.allocate_bytes(scalar_runtime.size(), scalar_runtime.alignment());
    scalar_runtime.construct_initial_value(storage);

    {
        auto context = bg::details::ScalarAllocationContext{scalar_allocator, *allocation_owner, nullptr, 0};
        auto time_series_state = bg::TimeSeriesState{};
        auto output_state = bg::details::OutputState{time_series_runtime};
        auto evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{1}};
        output_state.bind(time_series_state, storage, evaluation_time);
        auto output = bg::Out<bg::TS_T>{bg::TimeSeriesOutputBinding{output_state}};
        auto release_storage = bg::util::scope_exit{[&]() noexcept {
            scalar_runtime.destroy(storage, scalar_allocator);
            output_allocator.deallocate_bytes(storage, scalar_runtime.size(), scalar_runtime.alignment());
        }};

        output.apply(initial);
        verify(scalar_runtime, storage, 0, false);
        snapshot.emplace(bg::Scalar<>::from(scalar_runtime, storage), snapshot_allocator);
        verify(snapshot->runtime_type(), snapshot->view().data(), 0, true);

        evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{2}};
        output_state.bind(time_series_state, storage, evaluation_time);
        output.apply(replacement);
        verify(scalar_runtime, storage, 1, false);
        verify(snapshot->runtime_type(), snapshot->view().data(), 0, true);

        const auto last_updated = output.last_updated();
        evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{3}};
        output_state.bind(time_series_state, storage, evaluation_time);
        auto failed = false;
        try {
            output.apply(invalid_replacement);
        } catch (const std::invalid_argument &) {
            failed = true;
        }
        require(failed);
        require(output.last_updated() == last_updated);
        verify(scalar_runtime, storage, 1, false);
        verify(snapshot->runtime_type(), snapshot->view().data(), 0, true);
    }

    verify(snapshot->runtime_type(), snapshot->view().data(), 0, true);
    snapshot.reset();
    require(scalar_memory.outstanding_allocations() == 0);
    release_allocation_owner.release();
    allocation_owner->release();
}

void verify_nested_list_replacement_snapshots(const bg::ScalarRuntimeType &runtime, const bg::ScalarType &type) {
    const auto initial = std::vector<std::vector<i64>>{{1, 2}, {3}};
    const auto replacement = std::vector<std::vector<i64>>{{4}, {5, 6}};
    const auto verify = [&](const bg::ScalarRuntimeType &runtime_type, const void *storage, std::size_t version,
                            bool) {
        const auto &outer = static_cast<const bg::ScalarListRuntimeType &>(runtime_type);
        const auto &inner = static_cast<const bg::ScalarListRuntimeType &>(outer.item_runtime_type());
        const auto &expected = version == 0 ? initial : replacement;
        require(outer.item_count(storage) == expected.size());
        for (auto outer_index = std::size_t{}; outer_index < expected.size(); ++outer_index) {
            const auto *inner_storage = outer.item(storage, outer_index);
            require(inner.item_count(inner_storage) == expected[outer_index].size());
            for (auto inner_index = std::size_t{}; inner_index < expected[outer_index].size(); ++inner_index) {
                require(*static_cast<const i64 *>(inner.item(inner_storage, inner_index)) ==
                        expected[outer_index][inner_index]);
            }
        }
    };

    verify_retained_scalar_snapshot(runtime, type, {{1, 2}, {3}}, {{4}, {5, 6}}, {{7, 8}, {9, "bad"}}, verify);
}

void verify_shared_string_list_snapshot() {
    const auto &runtime = static_cast<const bg::ScalarListRuntimeType &>(
        bg::cpp_scalar_runtime_type<bg::List<bg::string>>());
    auto original_data = static_cast<const char *>(nullptr);
    const auto verify = [&](const bg::ScalarRuntimeType &runtime_type, const void *storage, std::size_t version,
                            bool snapshot) {
        const auto list = bg::Scalar<bg::List<bg::string>>::from(runtime_type, storage);
        const auto expected = version == 0 ? std::array<std::string_view, 2>{"old one", "old two"}
                                           : std::array<std::string_view, 2>{"new value", {}};
        const auto expected_size = version == 0 ? std::size_t{2} : std::size_t{1};
        require(list.size() == expected_size);
        if (version == 0) {
            if (snapshot) {
                require(list[0].value().data() == original_data);
            } else if (original_data == nullptr) {
                original_data = list[0].value().data();
            }
        } else {
            require(list[0].value().data() != original_data);
        }
        for (auto index = std::size_t{}; index < expected_size; ++index) {
            require(list[index].value() == expected[index]);
        }
    };

    verify_retained_scalar_snapshot(runtime, runtime.type(), {"old one", "old two"}, {"new value"},
                                    {"later value", 3}, verify);
}

void verify_composed_and_nested_list_outputs() {
    const auto &integer_runtime = bg::cpp_scalar_runtime_type<i64>();
    const auto &integer_list_type = bg::list_scalar_type(integer_runtime.type());
    const auto integer_available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &composed_integers = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(integer_list_type, integer_available));
    verify_direct_scalar_output(composed_integers, integer_list_type, {1, 2}, 1,
                              [](const auto &runtime, const void *storage) {
        require(runtime.item_count(storage) == 2);
        require(*static_cast<const i64 *>(runtime.item(storage, 0)) == 1);
        require(*static_cast<const i64 *>(runtime.item(storage, 1)) == 2);
    }, true);

    using Item = bg::Opaque<RuntimeListItem>;
    const auto &item_runtime = bg::cpp_scalar_runtime_type<Item>();
    const auto &list_type = bg::list_scalar_type(item_runtime.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item_runtime};
    const auto &composed = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(list_type, available));
    auto first = bg::ScalarValue<Item>{RuntimeListItem{11, 12}};
    auto second = bg::ScalarValue<Item>{RuntimeListItem{21, 22}};
    RuntimeListItem::copy_count = 0;
    verify_direct_scalar_output(composed, list_type, {first, second}, 1, [](const auto &runtime, const void *storage) {
        require(runtime.item_count(storage) == 2);
        const auto *first_value = static_cast<const RuntimeListItem *>(runtime.item(storage, 0));
        const auto *second_value = static_cast<const RuntimeListItem *>(runtime.item(storage, 1));
        require(first_value->first == 11);
        require(second_value->second == 22);
    });
    require(RuntimeListItem::copy_count == 2);

    const auto &native = static_cast<const bg::ScalarListRuntimeType &>(
        bg::cpp_scalar_runtime_type<bg::List<Item>>());
    RuntimeListItem::copy_count = 0;
    verify_direct_scalar_output(native, list_type, {first, second}, 1, [](const auto &runtime, const void *storage) {
        require(runtime.item_count(storage) == 2);
        const auto *first_value = static_cast<const RuntimeListItem *>(runtime.item(storage, 0));
        const auto *second_value = static_cast<const RuntimeListItem *>(runtime.item(storage, 1));
        require(first_value->first == 11);
        require(second_value->second == 22);
    });
    require(RuntimeListItem::copy_count == 2);

    const auto &native_integers = static_cast<const bg::ScalarListRuntimeType &>(
        bg::cpp_scalar_runtime_type<bg::List<i64>>());
    verify_direct_scalar_output(native_integers, native_integers.type(), {1, 2}, 1,
                              [](const auto &runtime, const void *storage) {
        require(runtime.item_count(storage) == 2);
        require(*static_cast<const i64 *>(runtime.item(storage, 0)) == 1);
        require(*static_cast<const i64 *>(runtime.item(storage, 1)) == 2);
    }, true);

    const auto &inner_type = bg::list_scalar_type(integer_runtime.type());
    const auto &middle_type = bg::list_scalar_type(inner_type);
    const auto &outer_type = bg::list_scalar_type(middle_type);
    const auto nested_available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &outer_runtime = static_cast<const bg::ScalarListRuntimeType &>(
        bg::resolve_scalar_runtime_type(outer_type, nested_available));
    verify_direct_scalar_output(outer_runtime, outer_type, {{{1, 2}, {3}}, {{4}}}, 6,
                              [](const auto &runtime, const void *storage) {
        require(runtime.item_count(storage) == 2);
        const auto &middle_runtime = static_cast<const bg::ScalarListRuntimeType &>(runtime.item_runtime_type());
        const auto &inner_runtime = static_cast<const bg::ScalarListRuntimeType &>(middle_runtime.item_runtime_type());
        const auto *first_middle = runtime.item(storage, 0);
        const auto *second_middle = runtime.item(storage, 1);
        require(middle_runtime.item_count(first_middle) == 2);
        require(middle_runtime.item_count(second_middle) == 1);
        const auto *first_inner = middle_runtime.item(first_middle, 0);
        const auto *second_inner = middle_runtime.item(first_middle, 1);
        const auto *third_inner = middle_runtime.item(second_middle, 0);
        require(inner_runtime.item_count(first_inner) == 2);
        require(inner_runtime.item_count(second_inner) == 1);
        require(inner_runtime.item_count(third_inner) == 1);
        require(*static_cast<const i64 *>(inner_runtime.item(first_inner, 1)) == 2);
        require(*static_cast<const i64 *>(inner_runtime.item(second_inner, 0)) == 3);
        require(*static_cast<const i64 *>(inner_runtime.item(third_inner, 0)) == 4);
    });

    verify_nested_list_replacement_snapshots(
        static_cast<const bg::ScalarListRuntimeType &>(bg::cpp_scalar_runtime_type<bg::List<bg::List<i64>>>()),
        bg::cpp_scalar_type<bg::List<bg::List<i64>>>());
    verify_nested_list_replacement_snapshots(
        static_cast<const bg::ScalarListRuntimeType &>(outer_runtime.item_runtime_type()), middle_type);
    verify_shared_string_list_snapshot();
}

void verify_native_and_composed_set_outputs() {
    const auto &integer_runtime = bg::cpp_scalar_runtime_type<i64>();
    using IntegerSet = bg::Set<i64>;
    const auto verify = [](const bg::ScalarRuntimeType &runtime, const void *storage, std::size_t version,
                           bool) {
        const auto values = bg::Scalar<IntegerSet>::from(runtime, storage);
        if (version == 0) {
            require(values.size() == 2 && values.contains(1) && values.contains(2));
        } else {
            require(values.size() == 1 && values.contains(4));
        }
    };

    const auto &native = bg::cpp_scalar_runtime_type<IntegerSet>();
    verify_direct_scalar_output(native, native.type(), {1, 2}, std::nullopt,
                                [&](const auto &runtime, const void *storage) { verify(runtime, storage, 0, false); },
                                true);
    verify_retained_scalar_snapshot(native, native.type(), {1, 2}, {4}, {7, "bad"}, verify);

    const auto &set_type = bg::set_scalar_type(integer_runtime.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &composed = bg::resolve_scalar_runtime_type(set_type, available);
    verify_direct_scalar_output(composed, set_type, {1, 2}, std::nullopt,
                                [&](const auto &runtime, const void *storage) { verify(runtime, storage, 0, false); },
                                true);
    verify_retained_scalar_snapshot(composed, set_type, {1, 2}, {4}, {7, "bad"}, verify);
}

void verify_native_and_composed_dictionary_outputs() {
    const auto &integer_runtime = bg::cpp_scalar_runtime_type<i64>();
    using IntegerDictionary = bg::Dict<i64, i64>;
    const auto verify_integer_dictionary = [](const bg::ScalarRuntimeType &runtime, const void *storage,
                                               std::size_t version, bool) {
        const auto values = bg::Scalar<IntegerDictionary>::from(runtime, storage);
        if (version == 0) {
            require(values.size() == 2 && values.at(1).value() == 2 && values.at(3).value() == 4);
        } else {
            require(values.size() == 1 && values.at(4).value() == 5);
        }
    };

    const auto &native = bg::cpp_scalar_runtime_type<IntegerDictionary>();
    verify_direct_scalar_output(native, native.type(), {{1, 2}, {3, 4}}, std::nullopt,
                                [&](const auto &runtime, const void *storage) {
                                    verify_integer_dictionary(runtime, storage, 0, false);
                                });
    verify_retained_scalar_snapshot(native, native.type(), {{1, 2}, {3, 4}}, {{4, 5}}, {{6, "bad"}},
                                    verify_integer_dictionary);

    const auto &integer_dictionary_type = bg::dict_scalar_type(integer_runtime.type(), integer_runtime.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &composed = bg::resolve_scalar_runtime_type(integer_dictionary_type, available);
    verify_direct_scalar_output(composed, integer_dictionary_type, {{1, 2}, {3, 4}}, std::nullopt,
                                [&](const auto &runtime, const void *storage) {
                                    verify_integer_dictionary(runtime, storage, 0, false);
                                });
    verify_retained_scalar_snapshot(composed, integer_dictionary_type, {{1, 2}, {3, 4}}, {{4, 5}}, {{6, "bad"}},
                                    verify_integer_dictionary);

    using NestedDictionary = bg::Dict<i64, bg::List<i64>>;
    const auto &integer_list_type = bg::list_scalar_type(integer_runtime.type());
    const auto list_available = std::array<const bg::ScalarRuntimeType *, 1>{&integer_runtime};
    const auto &composed_list = bg::resolve_scalar_runtime_type(integer_list_type, list_available);
    const auto &nested_dictionary_type = bg::dict_scalar_type(integer_runtime.type(), integer_list_type);
    const auto nested_available =
        std::array<const bg::ScalarRuntimeType *, 2>{&integer_runtime, &composed_list};
    const auto &composed_nested = bg::resolve_scalar_runtime_type(nested_dictionary_type, nested_available);
    const auto verify_nested_dictionary = [](const bg::ScalarRuntimeType &runtime, const void *storage,
                                             std::size_t version, bool) {
        const auto values = bg::Scalar<NestedDictionary>::from(runtime, storage);
        if (version == 0) {
            require(values.size() == 2 && values.at(1).size() == 2 && values.at(1)[1].value() == 2 &&
                    values.at(3)[0].value() == 4);
        } else {
            require(values.size() == 1 && values.at(4).size() == 2 && values.at(4)[1].value() == 6);
        }
    };

    const auto verify_nested_runtime = [&](const bg::ScalarRuntimeType &runtime,
                                           const literal::ScalarLiteral &initial,
                                           const literal::ScalarLiteral &invalid) {
        verify_direct_scalar_output(runtime, nested_dictionary_type, initial, std::nullopt,
                                    [&](const auto &type, const void *storage) {
                                        verify_nested_dictionary(type, storage, 0, false);
                                    });
        verify_retained_scalar_snapshot(runtime, nested_dictionary_type, initial, {{4, {5, 6}}}, invalid,
                                        verify_nested_dictionary);
    };
    verify_nested_runtime(bg::cpp_scalar_runtime_type<NestedDictionary>(), {{1, {1, 2}}, {3, {4}}},
                          {{7, {8, "bad"}}});
    verify_nested_runtime(composed_nested, {{1, {1, 2}}, {3, {4}}}, {{7, {8, "bad"}}});
}

void verify_native_compound_and_bundle_outputs() {
    const auto &compound_runtime = bg::cpp_scalar_runtime_type<LiteralCompound>();
    const auto verify_compound = [](const bg::ScalarRuntimeType &, const void *storage, std::size_t version, bool) {
        const auto &value = *static_cast<const LiteralCompound *>(storage);
        require(reinterpret_cast<std::uintptr_t>(std::addressof(value)) % alignof(LiteralCompound) == 0);
        if (version == 0) {
            require(value.first == 10 && value.second == 20);
        } else {
            require(value.first == 30 && value.second == 40);
        }
    };
    verify_direct_scalar_output(compound_runtime, compound_runtime.type(), {{"first", 10}, {"second", 20}}, 0,
                                [&](const auto &runtime, const void *storage) {
                                    verify_compound(runtime, storage, 0, false);
                                });
    verify_retained_scalar_snapshot(compound_runtime, compound_runtime.type(), {{"first", 10}, {"second", 20}},
                                    {{"first", 30}, {"second", 40}}, {{"first", 50}, {"second", "bad"}},
                                    verify_compound);

    using BundleValue = bg::Bundle<LiteralBundle>;
    using NestedDictionary = bg::Dict<bg::string, bg::List<i64>>;
    const auto &bundle_runtime = bg::cpp_scalar_runtime_type<BundleValue>();
    const auto verify_bundle = [](const bg::ScalarRuntimeType &runtime, const void *storage, std::size_t version,
                                  bool) {
        const auto bundle = bg::Scalar<BundleValue>::from(runtime, storage);
        const auto nested = bg::scalar_cast<NestedDictionary>(bundle[0]);
        const auto compound = bg::scalar_cast<LiteralCompound>(bundle[1]);
        if (version == 0) {
            require(nested.size() == 2 && nested.at("old").size() == 2 && nested.at("old")[1].value() == 2 &&
                    nested.at("keep")[0].value() == 3 && compound.value().first == 10 &&
                    compound.value().second == 20);
        } else {
            require(nested.size() == 1 && nested.at("new").size() == 2 && nested.at("new")[1].value() == 6 &&
                    compound.value().first == 30 && compound.value().second == 40);
        }
    };
    const auto verify_bundle_literals = [&](const literal::ScalarLiteral &initial,
                                           const literal::ScalarLiteral &replacement,
                                           const literal::ScalarLiteral &invalid) {
        verify_direct_scalar_output(bundle_runtime, bundle_runtime.type(), initial, std::nullopt,
                                    [&](const auto &runtime, const void *storage) {
                                        verify_bundle(runtime, storage, 0, false);
                                    });
        verify_retained_scalar_snapshot(bundle_runtime, bundle_runtime.type(), initial, replacement, invalid,
                                        verify_bundle);
    };
    verify_bundle_literals(
        {{"nested", {{"old", {1, 2}}, {"keep", {3}}}}, {"compound", {{"first", 10}, {"second", 20}}}},
        {{"nested", {{"new", {5, 6}}}}, {"compound", {{"first", 30}, {"second", 40}}}},
        {{"nested", {{"broken", {7, "bad"}}}}, {"compound", {{"first", 50}, {"second", 60}}}});
}

void verify_bundle_base_assignment_fallback() {
    using BundleValue = bg::Bundle<LiteralBundle>;
    using NestedDictionary = bg::Dict<bg::string, bg::List<i64>>;
    const auto &runtime = bg::cpp_scalar_runtime_type<BundleValue>();
    const auto verify_literal = [&](const literal::ScalarLiteral &literal) {
        auto scalar_memory = AllocationCounter{};
        auto scalar_allocator = bg::allocator{scalar_memory};
        auto owner_resource = std::pmr::unsynchronized_pool_resource{};
        auto owner_allocator = bg::allocator{owner_resource};
        auto *allocation_owner = bg::details::BundleAllocationOwner::create(owner_allocator);
        auto release_owner = bg::util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
        auto storage_resource = std::pmr::unsynchronized_pool_resource{};
        auto storage_allocator = bg::allocator{storage_resource};
        auto *storage = storage_allocator.allocate_bytes(runtime.size(), runtime.alignment());
        runtime.construct_initial_value(storage);
        auto release_storage = bg::util::scope_exit{[&]() noexcept {
            runtime.destroy(storage, scalar_allocator);
            storage_allocator.deallocate_bytes(storage, runtime.size(), runtime.alignment());
        }};
        auto snapshot_resource = std::pmr::unsynchronized_pool_resource{};
        auto snapshot_allocator = bg::allocator{snapshot_resource};
        auto snapshot = std::optional<bg::ScalarValue<>>{};

        {
            auto context = bg::details::ScalarAllocationContext{scalar_allocator, *allocation_owner, nullptr, 0};
            auto construction_context = bg::details::ScalarLiteralConstructionContext{literal, true};
            const auto construction = bg::ScalarConstruction{
                &construction_context, &bg::details::ScalarLiteralConstructionContext::callback};
            auto &base_runtime = static_cast<const bg::ScalarRuntimeType &>(runtime);
            require(base_runtime.bg::ScalarRuntimeType::assign_with(storage, construction, true));
            snapshot.emplace(bg::Scalar<>::from(runtime, storage), snapshot_allocator);
            require(!base_runtime.bg::ScalarRuntimeType::assign_with(storage, construction, true));
            const auto bundle = bg::Scalar<BundleValue>::from(runtime, storage);
            const auto snapshot_bundle = bg::scalar_cast<BundleValue>(snapshot->view());
            const auto current_nested = bg::scalar_cast<NestedDictionary>(bundle[0]);
            const auto old_nested = bg::scalar_cast<NestedDictionary>(snapshot_bundle[0]);
            require(current_nested.at("fallback")[1].value() == 2 && old_nested.at("fallback")[1].value() == 2);
        }

        snapshot.reset();
        release_storage.release();
        runtime.destroy(storage, scalar_allocator);
        storage_allocator.deallocate_bytes(storage, runtime.size(), runtime.alignment());
        require(scalar_memory.outstanding_allocations() == 0);
        release_owner.release();
        allocation_owner->release();
    };
    verify_literal({{"nested", {{"fallback", {1, 2}}}}, {"compound", {{"first", 11}, {"second", 22}}}});
}

void verify_provider_list_output() {
    const auto update = bg::node([](In<TS<i64>> input, Out<TS<bg::List<test_scalar_provider::value>>> output) {
        const auto value = test_scalar_provider::value{static_cast<int>(*input)};
        output.apply({value, value});
    });
    const auto observe = bg::node([](In<TS<bg::List<test_scalar_provider::value>>> input, Out<TS<i64>> output) {
        const auto values = input.value_view();
        output.set(values[0].value().get() + values[1].value().get());
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return observe(update(input)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 2>{6, 9});

    require(actual.equals<i64>({12, 18}));
}

void verify_provider_scalar_leaf() {
    const auto write = bg::node([](In<TS<i64>> input, Out<TS<test_scalar_provider::value>> output) {
        literal::apply(output, test_scalar_provider::value{static_cast<int>(*input)});
    });
    const auto read = bg::node([](In<TS<test_scalar_provider::value>> input, Out<TS<i64>> output) {
        output.set(static_cast<i64>((*input).get()));
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return read(write(input)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 2>{12, 17});

    require(actual.equals<i64>({12, 17}));
}

void verify_scalar_view_leaf() {
    const auto copy = bg::node([](In<TS<i64>> input, Out<TS<i64>> output) {
        literal::apply(output, input.value_view());
    });
    const auto graph = [&](bg::wp<TS<i64>> input) { return copy(input); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 2>{21, 34});

    require(actual.equals<i64>({21, 34}));
}

void verify_invalid_list_deltas() {
    const auto observe = bg::node([](In<bg::TS_T, bg::optional> input, Out<TS<i64>> output) {
        output.set(input.valid() ? 1 : 0);
    });

    const auto duplicate = bg::node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        literal::apply(output, {{0, *input}, {0, *input}});
    });
    const auto duplicate_graph = [&](bg::wp<TS<i64>> input) { return observe(duplicate(input)); };
    auto duplicate_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(duplicate_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected);

    const auto out_of_range = bg::node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        literal::apply(output, {{2, *input}});
    });
    const auto out_of_range_graph = [&](bg::wp<TS<i64>> input) { return observe(out_of_range(input)); };
    auto range_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(out_of_range_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        range_rejected = true;
    }
    require(range_rejected);

    const auto malformed = bg::node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        literal::apply(output, {{*input}});
    });
    const auto malformed_graph = [&](bg::wp<TS<i64>> input) { return observe(malformed(input)); };
    auto malformed_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(malformed_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        malformed_rejected = true;
    }
    require(malformed_rejected);

    const auto invalid_index = bg::node([](In<TS<i64>> input, Out<TSL<TS<i64>, 2>> output) {
        switch (*input) {
        case 0:
            literal::apply(output, {{-1, *input}});
            return;
        case 1:
            literal::apply(output, {{1.5, *input}});
            return;
        case 2:
            literal::apply(output, {{true, *input}});
            return;
        default:
            literal::apply(output, {{0, *input, *input}});
        }
    });
    const auto invalid_index_graph = [&](bg::wp<TS<i64>> input) { return observe(invalid_index(input)); };
    for (const auto value : std::array<i64, 4>{0, 1, 2, 3}) {
        auto rejected = false;
        try {
            static_cast<void>(bg::testing::test_graph(invalid_index_graph, std::array{i64{value}}));
        } catch (const std::invalid_argument &) {
            rejected = true;
        }
        require(rejected);
    }

    const auto unsupported_scalar_sequence = bg::node([](In<TS<i64>> input, Out<TS<i64>> output) {
        static_cast<void>(input);
        literal::apply(output, {{1, 2}});
    });
    const auto unsupported_scalar_sequence_graph = [&](bg::wp<TS<i64>> input) {
        return observe(unsupported_scalar_sequence(input));
    };
    auto scalar_sequence_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(unsupported_scalar_sequence_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        scalar_sequence_rejected = true;
    }
    require(scalar_sequence_rejected);

    const auto unsupported_set = bg::node([](In<TS<i64>> input, Out<bg::TSS<i64>> output) {
        static_cast<void>(input);
        literal::apply(output, {});
    });
    const auto unsupported_set_graph = [&](bg::wp<TS<i64>> input) { return observe(unsupported_set(input)); };
    auto set_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(unsupported_set_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        set_rejected = true;
    }
    require(set_rejected);

    const auto unsupported_dictionary = bg::node([](In<TS<i64>> input, Out<bg::TSD<i64, TS<i64>>> output) {
        static_cast<void>(input);
        literal::apply(output, {});
    });
    const auto unsupported_dictionary_graph = [&](bg::wp<TS<i64>> input) {
        return observe(unsupported_dictionary(input));
    };
    auto dictionary_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(unsupported_dictionary_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        dictionary_rejected = true;
    }
    require(dictionary_rejected);

    const auto unsupported_bundle = bg::node([](In<TS<i64>> input, Out<bg::TSB<ProbeBundle>> output) {
        static_cast<void>(input);
        literal::apply(output, {});
    });
    const auto unsupported_bundle_graph = [&](bg::wp<TS<i64>> input) { return observe(unsupported_bundle(input)); };
    auto bundle_rejected = false;
    try {
        static_cast<void>(bg::testing::test_graph(unsupported_bundle_graph, std::array<i64, 1>{1}));
    } catch (const std::invalid_argument &) {
        bundle_rejected = true;
    }
    require(bundle_rejected);
}

void verify_duplicate_prevalidation_and_partial_failure() {
    duplicate_prevalidation_preserved_output = false;
    const auto duplicate = bg::node([](In<TS<i64>> command, Out<TSL<TS<i64>, 2>> output) {
        try {
            literal::apply(output, {{0, *command}, {0, *command}});
        } catch (const std::invalid_argument &) {
            duplicate_prevalidation_preserved_output = !output.valid() && !output[0].valid();
        }
    });
    const auto observe = bg::node([](In<TS<i64>> command, In<TSL<TS<i64>, 2>, bg::optional> input,
                                     Out<TS<i64>> output) {
        output.set((input.valid() ? 100 : 0) + (input[0].valid() ? 1 : 0));
        static_cast<void>(command);
    });
    const auto graph = [&](bg::wp<TS<i64>> command) { return observe(command, duplicate(command)); };
    const auto actual = bg::testing::test_graph(graph, std::array<i64, 1>{9});
    require(actual.equals<i64>({0}));
    require(duplicate_prevalidation_preserved_output);

    partial_update_observed = false;
    const auto partial = bg::node([](In<TS<i64>> command, Out<TSL<TS<i64>, 2>> output) {
        try {
            literal::apply(output, {{0, *command}, {1, {4}}});
        } catch (const std::invalid_argument &) {
            partial_update_observed = output[0].valid() && output[0].value() == *command && !output[1].valid();
        }
    });
    const auto partial_graph = [&](bg::wp<TS<i64>> command) { return observe(command, partial(command)); };
    const auto partial_actual = bg::testing::test_graph(partial_graph, std::array<i64, 1>{9});
    require(partial_actual.equals<i64>({101}));
    require(partial_update_observed);
}

} // namespace

int main() {
    verify_scalar_leaf_and_deduplicated_output();
    verify_fixed_list_delta_and_notifications();
    verify_nested_list_delta();
    verify_runtime_output_dispatch();
    verify_empty_root_and_nested_deltas();
    verify_equal_deduplicated_value_revalidates();
    verify_scalar_list_output();
    verify_composed_and_nested_list_outputs();
    verify_native_and_composed_set_outputs();
    verify_native_and_composed_dictionary_outputs();
    verify_native_compound_and_bundle_outputs();
    verify_bundle_base_assignment_fallback();
    verify_provider_list_output();
    verify_provider_scalar_leaf();
    verify_scalar_view_leaf();
    verify_invalid_list_deltas();
    verify_duplicate_prevalidation_and_partial_failure();
}

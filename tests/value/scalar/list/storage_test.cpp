#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/scalar/details/allocation_pool.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../../utils/counting_memory_resource.hpp"

#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "../test_scalar_provider.hpp"

using namespace bg;

TEST_CASE("resolved scalar list storage supports mutation",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    const auto &item = cpp_scalar_runtime_type<string>();
    const auto &type = list_scalar_type(item.type());
    const auto available = std::array<const ScalarRuntimeType *, 1>{&item};
    const auto &list = static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(list.size(), list.alignment());
    list.construct_initial_value(storage);

    const auto first = ScalarValue{"first"};
    const auto second = ScalarValue{"second"};
    list.append(storage, first, allocator);
    list.append(storage, second, allocator);
    list.set(storage, 0, second);
    list.erase(storage, 1, allocator);

    REQUIRE(list.item_count(storage) == 1);
    CHECK(Scalar<string>::from(item, list.item(storage, 0)).value() == "second");

    list.clear(storage, allocator);
    CHECK(list.item_count(storage) == 0);

    list.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, list.size(), list.alignment());
}

TEST_CASE("the default scalar list assignment constructs before replacing provider values",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    using Value = test_scalar_provider::value;
    using List = List<Value>;

    const auto &runtime = cpp_scalar_runtime_type<List>();
    const auto &list_runtime = static_cast<const ScalarListRuntimeType &>(runtime);
    auto scalar_memory = counting_memory_resource{};
    auto scalar_allocator = allocator{scalar_memory};
    auto *allocation_owner = details::ScalarAllocationPool::create(scalar_allocator);
    auto release_allocation_owner = util::scope_exit{[&]() noexcept { allocation_owner->release(); }};
    auto value_memory = std::pmr::unsynchronized_pool_resource{};
    auto value_allocator = allocator{value_memory};
    auto *destination = value_allocator.allocate_bytes(runtime.size(), runtime.alignment());
    runtime.construct_initial_value(destination);

    auto destroy_destination = util::scope_exit{[&]() noexcept {
        runtime.destroy(destination, scalar_allocator);
        value_allocator.deallocate_bytes(destination, runtime.size(), runtime.alignment());
    }};

    const auto first_source = ScalarValue<List>{std::array<Value, 1>{Value{42}}};
    const auto first_view = first_source.view();
    auto first_data = details::scalar_data(first_view);
    const auto construct_from_source = [](void *context, const ScalarRuntimeType &runtime_type, void *target,
                                          allocator &memory) {
        runtime_type.construct_from_input(target, *static_cast<const details::ScalarData *>(context), memory);
    };
    const auto first_construction = ScalarConstruction{&first_data, construct_from_source};

    {
        auto context = details::ScalarAllocationContext{scalar_allocator, *allocation_owner, nullptr, 0};
        CHECK(list_runtime.ScalarListRuntimeType::assign_with(destination, first_construction, false));
        const auto values = Scalar<List>::from(runtime, destination);
        REQUIRE(values.size() == 1);
        CHECK(values[0].value().get() == 42);

        const auto fail = [](void *, const ScalarRuntimeType &, void *, allocator &) {
            throw std::runtime_error{"list construction failed"};
        };
        const auto allocations_before_failure = scalar_memory.allocations();
        CHECK_THROWS_AS(list_runtime.ScalarListRuntimeType::assign_with(destination, {nullptr, fail}, false),
                        std::runtime_error);
        CHECK(scalar_memory.allocations() == allocations_before_failure);
        CHECK(values[0].value().get() == 42);

        CHECK_FALSE(list_runtime.ScalarListRuntimeType::assign_with(destination, first_construction, true));
        CHECK(values[0].value().get() == 42);

        const auto second_source = ScalarValue<List>{std::array<Value, 1>{Value{43}}};
        const auto second_view = second_source.view();
        auto second_data = details::scalar_data(second_view);
        const auto second_construction = ScalarConstruction{&second_data, construct_from_source};
        CHECK(list_runtime.ScalarListRuntimeType::assign_with(destination, second_construction, false));
        const auto updated = Scalar<List>::from(runtime, destination);
        CHECK(updated[0].value().get() == 43);

        runtime.destroy(destination, scalar_allocator);
        value_allocator.deallocate_bytes(destination, runtime.size(), runtime.alignment());
        destroy_destination.release();
    }

    allocation_owner->release();
    release_allocation_owner.release();
    CHECK(scalar_memory.allocations() == 0);
}

namespace {

struct RuntimeListItem {
    i64 first;
    i64 second;
};

} // namespace

TEST_CASE("a scalar list copies between composed and C++ storage",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    using Item = Opaque<RuntimeListItem>;
    using List = List<Item>;

    const auto &item = cpp_scalar_runtime_type<Item>();
    const auto &type = list_scalar_type(item.type());
    const auto available = std::array<const ScalarRuntimeType *, 1>{&item};
    const auto &composed =
        static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(type, available));
    const auto source =
        ScalarValue<List>{std::vector<RuntimeListItem>{{.first = 1, .second = 2}, {.first = 3, .second = 4}}};
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(composed.size(), composed.alignment());
    composed.construct_from_input(storage, source, allocator);

    const auto composed_view = Scalar<List>::from(composed, storage);
    const auto copy = ScalarValue<List>{composed_view, allocator};

    REQUIRE(copy.view().size() == 2);
    CHECK(copy.view()[0].value().first == 1);
    CHECK(copy.view()[1].value().second == 4);

    composed.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, composed.size(), composed.alignment());
}

TEST_CASE("a scalar list copy releases retained items when allocation fails",
          "[value][scalar:list][scalar:string][feature:scalar-storage][storage:variable]") {
    using List = List<string>;

    const auto items = std::array<std::string_view, 16>{
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen"};
    const auto &runtime_type = cpp_scalar_runtime_type<List>();
    auto source_memory = counting_memory_resource{};
    auto copy_memory = counting_memory_resource{};
    auto destination_memory = std::pmr::unsynchronized_pool_resource{};
    auto *destination = destination_memory.allocate(runtime_type.size(), runtime_type.alignment());

    {
        auto source_allocator = allocator{source_memory};
        const auto source = ScalarValue<List>{items, source_allocator};
        const auto source_view = source.view();

        SECTION("stored copy") {
            copy_memory.fail_after(1);
            auto copy_allocator = allocator{copy_memory};

            CHECK_THROWS_AS(runtime_type.copy_construct(destination, source_view.data(), copy_allocator),
                            std::bad_alloc);
        }

        SECTION("native value construction") {
            const auto &source_storage =
                *static_cast<const scalar_storage_traits<List>::stored_type *>(source_view.data());
            const auto source_list = scalar_storage_traits<List>::read(source_storage);
            copy_memory.fail_after(1);
            auto copy_allocator = allocator{copy_memory};

            CHECK_THROWS_AS(runtime_type.construct_from_value(destination, &source_list, copy_allocator),
                            std::bad_alloc);
        }

        CHECK(copy_memory.allocations() == 0);
    }

    destination_memory.deallocate(destination, runtime_type.size(), runtime_type.alignment());
    CHECK(source_memory.allocations() == 0);
}

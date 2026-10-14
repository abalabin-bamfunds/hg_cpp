#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include "../../../utils/counting_memory_resource.hpp"

#include <array>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <string_view>
#include <vector>

TEST_CASE("resolved scalar list storage supports mutation",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    const auto &item = bg::cpp_scalar_runtime_type<bg::string>();
    const auto &type = bg::list_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item};
    const auto &list = static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(list.size(), list.alignment());
    list.construct_initial_value(storage);

    const auto first = bg::ScalarValue{"first"};
    const auto second = bg::ScalarValue{"second"};
    list.append(storage, first, allocator);
    list.append(storage, second, allocator);
    list.set(storage, 0, second);
    list.erase(storage, 1, allocator);

    REQUIRE(list.item_count(storage) == 1);
    CHECK(bg::Scalar<bg::string>::from(item, list.item(storage, 0)).value() == "second");

    list.clear(storage, allocator);
    CHECK(list.item_count(storage) == 0);

    list.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, list.size(), list.alignment());
}

namespace {

struct RuntimeListItem {
    std::int64_t first;
    std::int64_t second;
};

} // namespace

TEST_CASE("a scalar list copies between composed and C++ storage",
          "[value][scalar][list][scalar:list][storage][feature:scalar-storage]") {
    using Item = bg::Opaque<RuntimeListItem>;
    using List = bg::List<Item>;

    const auto &item = bg::cpp_scalar_runtime_type<Item>();
    const auto &type = bg::list_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item};
    const auto &composed =
        static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    const auto source =
        bg::ScalarValue<List>{std::vector<RuntimeListItem>{{.first = 1, .second = 2}, {.first = 3, .second = 4}}};
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(composed.size(), composed.alignment());
    composed.construct_from_input(storage, source, allocator);

    const auto composed_view = bg::Scalar<List>::from(composed, storage);
    const auto copy = bg::ScalarValue<List>{composed_view, allocator};

    REQUIRE(copy.view().size() == 2);
    CHECK(copy.view()[0].value().first == 1);
    CHECK(copy.view()[1].value().second == 4);

    composed.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, composed.size(), composed.alignment());
}

TEST_CASE("a scalar list copy releases retained items when allocation fails",
          "[value][scalar:list][scalar:string][feature:scalar-storage][storage:variable]") {
    using List = bg::List<bg::string>;

    const auto items = std::array<std::string_view, 16>{
        "zero", "one", "two", "three", "four", "five", "six", "seven",
        "eight", "nine", "ten", "eleven", "twelve", "thirteen", "fourteen", "fifteen"};
    const auto &runtime_type = bg::cpp_scalar_runtime_type<List>();
    auto source_memory = counting_memory_resource{};
    auto copy_memory = counting_memory_resource{};
    auto destination_memory = std::pmr::unsynchronized_pool_resource{};
    auto *destination = destination_memory.allocate(runtime_type.size(), runtime_type.alignment());

    {
        auto source_allocator = bg::allocator{source_memory};
        const auto source = bg::ScalarValue<List>{items, source_allocator};
        const auto source_view = source.view();

        SECTION("stored copy") {
            copy_memory.fail_after(1);
            auto copy_allocator = bg::allocator{copy_memory};

            CHECK_THROWS_AS(runtime_type.copy_construct(destination, source_view.data(), copy_allocator),
                            std::bad_alloc);
        }

        SECTION("native value construction") {
            const auto &source_storage =
                *static_cast<const bg::scalar_storage_traits<List>::stored_type *>(source_view.data());
            const auto source_list = bg::scalar_storage_traits<List>::read(source_storage);
            copy_memory.fail_after(1);
            auto copy_allocator = bg::allocator{copy_memory};

            CHECK_THROWS_AS(runtime_type.construct_from_value(destination, &source_list, copy_allocator),
                            std::bad_alloc);
        }

        CHECK(copy_memory.allocations() == 0);
    }

    destination_memory.deallocate(destination, runtime_type.size(), runtime_type.alignment());
    CHECK(source_memory.allocations() == 0);
}

#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/storage_representation.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory_resource>

TEST_CASE("resolved scalar list storage supports mutation", "[value][scalar][list][storage]") {
    const auto &item = bg::cpp_scalar_storage_representation<bg::string>();
    const auto &type = bg::list_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarStorageRepresentation *, 1>{&item};
    const auto &list = static_cast<const bg::ScalarListStorageRepresentation &>(
        bg::resolve_scalar_storage_representation(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(list.size(), list.alignment());
    list.construct_initial_value(storage);

    const auto first = bg::ScalarValue{"first"};
    const auto second = bg::ScalarValue{"second"};
    list.append(storage, first.input_view(), memory);
    list.append(storage, second.input_view(), memory);
    list.set(storage, 0, second.input_view());
    list.erase(storage, 1, memory);

    REQUIRE(list.item_count(storage) == 1);
    CHECK(*static_cast<const std::pmr::string *>(list.item(storage, 0)) == "second");

    list.clear(storage, memory);
    CHECK(list.item_count(storage) == 0);

    list.destroy(storage, memory);
    memory.deallocate(storage, list.size(), list.alignment());
}

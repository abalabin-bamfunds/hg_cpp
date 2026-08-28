#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/dict/storage_representation.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/list/storage_representation.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <string_view>

TEST_CASE("resolved scalar dictionary storage supports mutation", "[value][scalar][dict][storage]") {
    const auto &key_storage = bg::cpp_scalar_storage_representation<std::int64_t>();
    const auto &value_storage = bg::cpp_scalar_storage_representation<bg::string>();
    const auto available = std::array<const bg::ScalarStorageRepresentation *, 2>{&key_storage, &value_storage};
    const auto &type = bg::dict_scalar_type(key_storage.type(), value_storage.type());
    const auto &dict = static_cast<const bg::ScalarDictStorageRepresentation &>(
        bg::resolve_scalar_storage_representation(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = bg::ScalarValue{1};
    const auto first = bg::ScalarValue{"first"};
    const auto second = bg::ScalarValue{"second"};
    dict.insert_or_assign(storage, key.input_view(), first.input_view(), memory);
    dict.insert_or_assign(storage, key.input_view(), second.input_view(), memory);

    REQUIRE(dict.item_count(storage) == 1);
    CHECK(dict.contains(storage, key.input_view()));
    CHECK(*static_cast<const std::pmr::string *>(dict.at(storage, key.input_view()).data) == "second");
    CHECK(dict.erase(storage, key.input_view(), memory));
    CHECK_FALSE(dict.erase(storage, key.input_view(), memory));

    dict.clear(storage, memory);
    CHECK(dict.item_count(storage) == 0);

    dict.destroy(storage, memory);
    memory.deallocate(storage, dict.size(), dict.alignment());
}

TEST_CASE("resolved dictionary storage owns nested collection values", "[value][scalar][dict][storage]") {
    const auto &key_storage = bg::cpp_scalar_storage_representation<std::int64_t>();
    const auto &item_storage = bg::cpp_scalar_storage_representation<bg::string>();
    const auto available = std::array<const bg::ScalarStorageRepresentation *, 2>{&key_storage, &item_storage};
    const auto &list_type = bg::list_scalar_type(item_storage.type());
    const auto &dict_type = bg::dict_scalar_type(key_storage.type(), list_type);
    const auto &dict = static_cast<const bg::ScalarDictStorageRepresentation &>(
        bg::resolve_scalar_storage_representation(dict_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = bg::ScalarValue{1};
    const auto values = bg::ScalarValue<bg::List<bg::string>>{std::array<std::string_view, 2>{"one", "two"}};
    dict.insert_or_assign(storage, key.input_view(), values.input_view(), memory);

    const auto stored = dict.at(storage, key.input_view());
    const auto &list = static_cast<const bg::ScalarListStorageRepresentation &>(stored.representation);
    REQUIRE(list.item_count(stored.data) == 2);
    CHECK(*static_cast<const std::pmr::string *>(list.item(stored.data, 1)) == "two");

    dict.destroy(storage, memory);
    memory.deallocate(storage, dict.size(), dict.alignment());
}

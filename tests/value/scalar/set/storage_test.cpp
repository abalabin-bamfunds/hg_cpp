#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/set/storage_representation.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>

TEST_CASE("resolved scalar set storage supports mutation", "[value][scalar][set][storage]") {
    const auto &item = bg::cpp_scalar_storage_representation<std::int64_t>();
    const auto &type = bg::set_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarStorageRepresentation *, 1>{&item};
    const auto &set = static_cast<const bg::ScalarSetStorageRepresentation &>(
        bg::resolve_scalar_storage_representation(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(set.size(), set.alignment());
    set.construct_initial_value(storage);

    const auto one = bg::ScalarValue{1};
    const auto two = bg::ScalarValue{2};
    CHECK(set.insert(storage, one.input_view(), memory));
    CHECK_FALSE(set.insert(storage, one.input_view(), memory));
    CHECK(set.insert(storage, two.input_view(), memory));
    CHECK(set.contains(storage, one.input_view()));
    CHECK(set.erase(storage, one.input_view(), memory));
    CHECK_FALSE(set.erase(storage, one.input_view(), memory));

    set.clear(storage, memory);
    CHECK(set.item_count(storage) == 0);

    set.destroy(storage, memory);
    memory.deallocate(storage, set.size(), set.alignment());
}

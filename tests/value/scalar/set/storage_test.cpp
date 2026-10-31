#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/runtime_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>

using namespace bg;

TEST_CASE("resolved scalar set storage supports mutation",
          "[value][scalar][set][scalar:set][storage][feature:scalar-storage]") {
    const auto &item = cpp_scalar_runtime_type<i64>();
    const auto &type = set_scalar_type(item.type());
    const auto available = std::array<const ScalarRuntimeType *, 1>{&item};
    const auto &set = static_cast<const ScalarSetRuntimeType &>(resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(set.size(), set.alignment());
    set.construct_initial_value(storage);

    const auto one = ScalarValue{1};
    const auto two = ScalarValue{2};
    CHECK(set.insert(storage, one, allocator));
    CHECK_FALSE(set.insert(storage, one, allocator));
    CHECK(set.insert(storage, two, allocator));
    CHECK(set.contains(storage, one));
    CHECK(set.erase(storage, one, allocator));
    CHECK_FALSE(set.erase(storage, one, allocator));

    set.clear(storage, allocator);
    CHECK(set.item_count(storage) == 0);

    set.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, set.size(), set.alignment());
}

TEST_CASE("a scalar set copies nested items between composed and C++ storage",
          "[value][scalar][set][scalar:set][storage][feature:scalar-storage]") {
    using Item = List<EvaluationTime>;
    using Set = Set<Item>;

    const auto &time = cpp_scalar_runtime_type<EvaluationTime>();
    const auto available = std::array<const ScalarRuntimeType *, 1>{&time};
    const auto &item_type = list_scalar_type(time.type());
    const auto &composed_item =
        static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(item_type, available));
    const auto &set_type = set_scalar_type(item_type);
    const auto &composed_set =
        static_cast<const ScalarSetRuntimeType &>(resolve_scalar_runtime_type(set_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *item_storage = allocator.allocate_bytes(composed_item.size(), composed_item.alignment());
    composed_item.construct_initial_value(item_storage);
    const auto time_value = ScalarValue{EvaluationTime{EvaluationTime::duration{1}}};
    composed_item.append(item_storage, time_value, allocator);

    auto *set_storage = allocator.allocate_bytes(composed_set.size(), composed_set.alignment());
    composed_set.construct_initial_value(set_storage);
    const auto item = Scalar<Item>::from(composed_item, item_storage);
    REQUIRE(composed_set.insert(set_storage, item, allocator));

    const auto &native_set = cpp_scalar_runtime_type<Set>();
    auto *native_storage = allocator.allocate_bytes(native_set.size(), native_set.alignment());
    const auto set_view = Scalar<Set>::from(composed_set, set_storage);
    native_set.construct_from_input(native_storage, set_view, allocator);

    CHECK(static_cast<const ScalarSetRuntimeType &>(native_set).contains(native_storage, item));

    native_set.destroy(native_storage, allocator);
    allocator.deallocate_bytes(native_storage, native_set.size(), native_set.alignment());
    composed_set.destroy(set_storage, allocator);
    allocator.deallocate_bytes(set_storage, composed_set.size(), composed_set.alignment());
    composed_item.destroy(item_storage, allocator);
    allocator.deallocate_bytes(item_storage, composed_item.size(), composed_item.alignment());
}

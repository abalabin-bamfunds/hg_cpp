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

TEST_CASE("resolved scalar set storage supports mutation",
          "[value][scalar][set][scalar:set][storage][feature:scalar-storage]") {
    const auto &item = bg::cpp_scalar_runtime_type<std::int64_t>();
    const auto &type = bg::set_scalar_type(item.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&item};
    const auto &set = static_cast<const bg::ScalarSetRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(set.size(), set.alignment());
    set.construct_initial_value(storage);

    const auto one = bg::ScalarValue{1};
    const auto two = bg::ScalarValue{2};
    CHECK(set.insert(storage, one, memory));
    CHECK_FALSE(set.insert(storage, one, memory));
    CHECK(set.insert(storage, two, memory));
    CHECK(set.contains(storage, one));
    CHECK(set.erase(storage, one, memory));
    CHECK_FALSE(set.erase(storage, one, memory));

    set.clear(storage, memory);
    CHECK(set.item_count(storage) == 0);

    set.destroy(storage, memory);
    memory.deallocate(storage, set.size(), set.alignment());
}

TEST_CASE("a scalar set copies nested items between composed and C++ storage",
          "[value][scalar][set][scalar:set][storage][feature:scalar-storage]") {
    using Item = bg::List<bg::EvaluationTime>;
    using Set = bg::Set<Item>;

    const auto &time = bg::cpp_scalar_runtime_type<bg::EvaluationTime>();
    const auto available = std::array<const bg::ScalarRuntimeType *, 1>{&time};
    const auto &item_type = bg::list_scalar_type(time.type());
    const auto &composed_item =
        static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(item_type, available));
    const auto &set_type = bg::set_scalar_type(item_type);
    const auto &composed_set =
        static_cast<const bg::ScalarSetRuntimeType &>(bg::resolve_scalar_runtime_type(set_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *item_storage = memory.allocate(composed_item.size(), composed_item.alignment());
    composed_item.construct_initial_value(item_storage);
    const auto time_value = bg::ScalarValue{bg::EvaluationTime{bg::EvaluationTime::duration{1}}};
    composed_item.append(item_storage, time_value, memory);

    auto *set_storage = memory.allocate(composed_set.size(), composed_set.alignment());
    composed_set.construct_initial_value(set_storage);
    const auto item = bg::Scalar<Item>::from(composed_item, item_storage);
    REQUIRE(composed_set.insert(set_storage, item, memory));

    const auto &native_set = bg::cpp_scalar_runtime_type<Set>();
    auto *native_storage = memory.allocate(native_set.size(), native_set.alignment());
    const auto set_view = bg::Scalar<Set>::from(composed_set, set_storage);
    native_set.construct_from_input(native_storage, set_view, memory);

    CHECK(static_cast<const bg::ScalarSetRuntimeType &>(native_set).contains(native_storage, item));

    native_set.destroy(native_storage, memory);
    memory.deallocate(native_storage, native_set.size(), native_set.alignment());
    composed_set.destroy(set_storage, memory);
    memory.deallocate(set_storage, composed_set.size(), composed_set.alignment());
    composed_item.destroy(item_storage, memory);
    memory.deallocate(item_storage, composed_item.size(), composed_item.alignment());
}

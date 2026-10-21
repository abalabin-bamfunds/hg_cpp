#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <utility>
#include <vector>

using namespace bg;

TEST_CASE("resolved scalar dictionary storage supports mutation",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    const auto &key_storage = cpp_scalar_runtime_type<i64>();
    const auto &value_storage = cpp_scalar_runtime_type<string>();
    const auto available = std::array<const ScalarRuntimeType *, 2>{&key_storage, &value_storage};
    const auto &type = dict_scalar_type(key_storage.type(), value_storage.type());
    const auto &dict = static_cast<const ScalarDictRuntimeType &>(resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = ScalarValue{1};
    const auto first = ScalarValue{"first"};
    const auto second = ScalarValue{"second"};
    dict.insert_or_assign(storage, key, first, allocator);
    dict.insert_or_assign(storage, key, second, allocator);

    REQUIRE(dict.item_count(storage) == 1);
    CHECK(dict.contains(storage, key));
    CHECK(Scalar<string>::from(value_storage, dict.at(storage, key).data).value() == "second");
    CHECK(dict.erase(storage, key, allocator));
    CHECK_FALSE(dict.erase(storage, key, allocator));

    dict.clear(storage, allocator);
    CHECK(dict.item_count(storage) == 0);

    dict.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, dict.size(), dict.alignment());
}

TEST_CASE("resolved dictionary storage owns nested collection values",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    const auto &key_storage = cpp_scalar_runtime_type<i64>();
    const auto &item_storage = cpp_scalar_runtime_type<string>();
    const auto available = std::array<const ScalarRuntimeType *, 2>{&key_storage, &item_storage};
    const auto &list_type = list_scalar_type(item_storage.type());
    const auto &dict_type = dict_scalar_type(key_storage.type(), list_type);
    const auto &dict = static_cast<const ScalarDictRuntimeType &>(resolve_scalar_runtime_type(dict_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = ScalarValue{1};
    const auto values = ScalarValue<List<string>>{std::array<std::string_view, 2>{"one", "two"}};
    dict.insert_or_assign(storage, key, values, allocator);

    const auto stored = dict.at(storage, key);
    const auto &list = static_cast<const ScalarListRuntimeType &>(stored.runtime_type);
    REQUIRE(list.item_count(stored.data) == 2);
    CHECK(Scalar<string>::from(item_storage, list.item(stored.data, 1)).value() == "two");

    dict.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, dict.size(), dict.alignment());
}

namespace {

struct RuntimeDictValue {
    i64 first;
    i64 second;
};

} // namespace

TEST_CASE("a scalar dictionary copies between composed and C++ storage",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    using Value = Opaque<RuntimeDictValue>;
    using Dict = Dict<i64, Value>;

    const auto &key = cpp_scalar_runtime_type<i64>();
    const auto &mapped = cpp_scalar_runtime_type<Value>();
    const auto &type = dict_scalar_type(key.type(), mapped.type());
    const auto available = std::array<const ScalarRuntimeType *, 2>{&key, &mapped};
    const auto &composed = static_cast<const ScalarDictRuntimeType &>(resolve_scalar_runtime_type(type, available));
    const auto source =
        ScalarValue<Dict>{std::vector<std::pair<i64, RuntimeDictValue>>{{1, {.first = 2, .second = 3}}}};
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};
    auto *storage = allocator.allocate_bytes(composed.size(), composed.alignment());
    composed.construct_from_input(storage, source, allocator);

    const auto composed_view = Scalar<Dict>::from(composed, storage);
    const auto found = composed_view.find(1);
    REQUIRE(found.has_value());
    CHECK(found->value().second == 3);

    auto cursor = composed_view.make_cursor();
    auto batch = std::array<ScalarDictRuntimeType::BatchEntry, 2>{};
    REQUIRE(cursor->next_batch(batch) == 1);
    CHECK(Scalar<i64>::from(composed.key_runtime_type(), batch[0].key).value() == 1);
    CHECK(Scalar<Value>::from(composed.mapped_runtime_type(), batch[0].mapped).value().second == 3);
    CHECK(cursor->next_batch(batch) == 0);

    const auto copy = ScalarValue<Dict>{composed_view, allocator};

    REQUIRE(copy.view().size() == 1);
    CHECK(copy.view().at(1).value().first == 2);
    CHECK(copy.view().at(1).value().second == 3);

    composed.destroy(storage, allocator);
    allocator.deallocate_bytes(storage, composed.size(), composed.alignment());
}

TEST_CASE("a scalar dictionary copies nested keys between composed and C++ storage",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    using InnerKey = List<EvaluationTime>;
    using Key = List<InnerKey>;
    using Dict = Dict<Key, i64>;

    const auto &time = cpp_scalar_runtime_type<EvaluationTime>();
    const auto &integer = cpp_scalar_runtime_type<i64>();
    const auto available = std::array<const ScalarRuntimeType *, 2>{&time, &integer};
    const auto &inner_type = list_scalar_type(time.type());
    const auto &inner = static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(inner_type, available));
    const auto &key_type = list_scalar_type(inner_type);
    const auto &key = static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(key_type, available));
    const auto &dict_type = dict_scalar_type(key_type, integer.type());
    const auto &composed =
        static_cast<const ScalarDictRuntimeType &>(resolve_scalar_runtime_type(dict_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocator = bg::allocator{memory};

    auto *inner_storage = allocator.allocate_bytes(inner.size(), inner.alignment());
    inner.construct_initial_value(inner_storage);
    const auto time_value = ScalarValue{EvaluationTime{EvaluationTime::duration{1}}};
    inner.append(inner_storage, time_value, allocator);
    const auto inner_view = Scalar<InnerKey>::from(inner, inner_storage);

    auto *key_storage = allocator.allocate_bytes(key.size(), key.alignment());
    key.construct_initial_value(key_storage);
    key.append(key_storage, inner_view, allocator);
    const auto key_view = Scalar<Key>::from(key, key_storage);

    auto *dict_storage = allocator.allocate_bytes(composed.size(), composed.alignment());
    composed.construct_initial_value(dict_storage);
    const auto mapped = ScalarValue{42};
    composed.insert_or_assign(dict_storage, key_view, mapped, allocator);

    const auto &native = cpp_scalar_runtime_type<Dict>();
    auto *native_storage = allocator.allocate_bytes(native.size(), native.alignment());
    const auto dict_view = Scalar<Dict>::from(composed, dict_storage);
    native.construct_from_input(native_storage, dict_view, allocator);
    const auto &native_dict = static_cast<const ScalarDictRuntimeType &>(native);

    REQUIRE(native_dict.contains(native_storage, key_view));
    CHECK(*static_cast<const i64 *>(native_dict.at(native_storage, key_view).data) == 42);

    native.destroy(native_storage, allocator);
    allocator.deallocate_bytes(native_storage, native.size(), native.alignment());
    composed.destroy(dict_storage, allocator);
    allocator.deallocate_bytes(dict_storage, composed.size(), composed.alignment());
    key.destroy(key_storage, allocator);
    allocator.deallocate_bytes(key_storage, key.size(), key.alignment());
    inner.destroy(inner_storage, allocator);
    allocator.deallocate_bytes(inner_storage, inner.size(), inner.alignment());
}

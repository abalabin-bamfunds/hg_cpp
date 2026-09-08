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

TEST_CASE("resolved scalar dictionary storage supports mutation",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    const auto &key_storage = bg::cpp_scalar_runtime_type<std::int64_t>();
    const auto &value_storage = bg::cpp_scalar_runtime_type<bg::string>();
    const auto available = std::array<const bg::ScalarRuntimeType *, 2>{&key_storage, &value_storage};
    const auto &type = bg::dict_scalar_type(key_storage.type(), value_storage.type());
    const auto &dict = static_cast<const bg::ScalarDictRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = bg::ScalarValue{1};
    const auto first = bg::ScalarValue{"first"};
    const auto second = bg::ScalarValue{"second"};
    dict.insert_or_assign(storage, key, first, memory);
    dict.insert_or_assign(storage, key, second, memory);

    REQUIRE(dict.item_count(storage) == 1);
    CHECK(dict.contains(storage, key));
    CHECK(*static_cast<const std::pmr::string *>(dict.at(storage, key).data) == "second");
    CHECK(dict.erase(storage, key, memory));
    CHECK_FALSE(dict.erase(storage, key, memory));

    dict.clear(storage, memory);
    CHECK(dict.item_count(storage) == 0);

    dict.destroy(storage, memory);
    memory.deallocate(storage, dict.size(), dict.alignment());
}

TEST_CASE("resolved dictionary storage owns nested collection values",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    const auto &key_storage = bg::cpp_scalar_runtime_type<std::int64_t>();
    const auto &item_storage = bg::cpp_scalar_runtime_type<bg::string>();
    const auto available = std::array<const bg::ScalarRuntimeType *, 2>{&key_storage, &item_storage};
    const auto &list_type = bg::list_scalar_type(item_storage.type());
    const auto &dict_type = bg::dict_scalar_type(key_storage.type(), list_type);
    const auto &dict =
        static_cast<const bg::ScalarDictRuntimeType &>(bg::resolve_scalar_runtime_type(dict_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(dict.size(), dict.alignment());
    dict.construct_initial_value(storage);

    const auto key = bg::ScalarValue{1};
    const auto values = bg::ScalarValue<bg::List<bg::string>>{std::array<std::string_view, 2>{"one", "two"}};
    dict.insert_or_assign(storage, key, values, memory);

    const auto stored = dict.at(storage, key);
    const auto &list = static_cast<const bg::ScalarListRuntimeType &>(stored.runtime_type);
    REQUIRE(list.item_count(stored.data) == 2);
    CHECK(*static_cast<const std::pmr::string *>(list.item(stored.data, 1)) == "two");

    dict.destroy(storage, memory);
    memory.deallocate(storage, dict.size(), dict.alignment());
}

namespace {

struct RuntimeDictValue {
    std::int64_t first;
    std::int64_t second;
};

} // namespace

TEST_CASE("a scalar dictionary copies between composed and C++ storage",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    using Value = bg::Opaque<RuntimeDictValue>;
    using Dict = bg::Dict<std::int64_t, Value>;

    const auto &key = bg::cpp_scalar_runtime_type<std::int64_t>();
    const auto &mapped = bg::cpp_scalar_runtime_type<Value>();
    const auto &type = bg::dict_scalar_type(key.type(), mapped.type());
    const auto available = std::array<const bg::ScalarRuntimeType *, 2>{&key, &mapped};
    const auto &composed =
        static_cast<const bg::ScalarDictRuntimeType &>(bg::resolve_scalar_runtime_type(type, available));
    const auto source =
        bg::ScalarValue<Dict>{std::vector<std::pair<std::int64_t, RuntimeDictValue>>{{1, {.first = 2, .second = 3}}}};
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto *storage = memory.allocate(composed.size(), composed.alignment());
    composed.construct_from_input(storage, source, memory);

    const auto composed_view = bg::Scalar<Dict>::from(composed, storage);
    const auto copy = bg::ScalarValue<Dict>{composed_view, memory};

    REQUIRE(copy.view().size() == 1);
    CHECK(copy.view().at(1).value().first == 2);
    CHECK(copy.view().at(1).value().second == 3);

    composed.destroy(storage, memory);
    memory.deallocate(storage, composed.size(), composed.alignment());
}

TEST_CASE("a scalar dictionary copies nested keys between composed and C++ storage",
          "[value][scalar][dict][scalar:dict][storage][feature:scalar-storage]") {
    using InnerKey = bg::List<bg::EvaluationTime>;
    using Key = bg::List<InnerKey>;
    using Dict = bg::Dict<Key, std::int64_t>;

    const auto &time = bg::cpp_scalar_runtime_type<bg::EvaluationTime>();
    const auto &integer = bg::cpp_scalar_runtime_type<std::int64_t>();
    const auto available = std::array<const bg::ScalarRuntimeType *, 2>{&time, &integer};
    const auto &inner_type = bg::list_scalar_type(time.type());
    const auto &inner =
        static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(inner_type, available));
    const auto &key_type = bg::list_scalar_type(inner_type);
    const auto &key =
        static_cast<const bg::ScalarListRuntimeType &>(bg::resolve_scalar_runtime_type(key_type, available));
    const auto &dict_type = bg::dict_scalar_type(key_type, integer.type());
    const auto &composed =
        static_cast<const bg::ScalarDictRuntimeType &>(bg::resolve_scalar_runtime_type(dict_type, available));
    auto memory = std::pmr::unsynchronized_pool_resource{};

    auto *inner_storage = memory.allocate(inner.size(), inner.alignment());
    inner.construct_initial_value(inner_storage);
    const auto time_value = bg::ScalarValue{bg::EvaluationTime{bg::EvaluationTime::duration{1}}};
    inner.append(inner_storage, time_value, memory);
    const auto inner_view = bg::Scalar<InnerKey>::from(inner, inner_storage);

    auto *key_storage = memory.allocate(key.size(), key.alignment());
    key.construct_initial_value(key_storage);
    key.append(key_storage, inner_view, memory);
    const auto key_view = bg::Scalar<Key>::from(key, key_storage);

    auto *dict_storage = memory.allocate(composed.size(), composed.alignment());
    composed.construct_initial_value(dict_storage);
    const auto mapped = bg::ScalarValue{42};
    composed.insert_or_assign(dict_storage, key_view, mapped, memory);

    const auto &native = bg::cpp_scalar_runtime_type<Dict>();
    auto *native_storage = memory.allocate(native.size(), native.alignment());
    const auto dict_view = bg::Scalar<Dict>::from(composed, dict_storage);
    native.construct_from_input(native_storage, dict_view, memory);
    const auto &native_dict = static_cast<const bg::ScalarDictRuntimeType &>(native);

    REQUIRE(native_dict.contains(native_storage, key_view));
    CHECK(*static_cast<const std::int64_t *>(native_dict.at(native_storage, key_view).data) == 42);

    native.destroy(native_storage, memory);
    memory.deallocate(native_storage, native.size(), native.alignment());
    composed.destroy(dict_storage, memory);
    memory.deallocate(dict_storage, composed.size(), composed.alignment());
    key.destroy(key_storage, memory);
    memory.deallocate(key_storage, key.size(), key.alignment());
    inner.destroy(inner_storage, memory);
    memory.deallocate(inner_storage, inner.size(), inner.alignment());
}

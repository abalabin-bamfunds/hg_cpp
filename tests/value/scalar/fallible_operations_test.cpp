#include <bg/scalar/dict/type.hpp>
#include <bg/scalar/list/type.hpp>
#include <bg/scalar/set/type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tss/tss_storage.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <vector>

namespace test_scalar {

inline bool throw_on_hash{};
inline bool throw_on_equality{};

struct fallible_value {
    explicit fallible_value(bg::i64 value) : value{value} {}

    bg::i64 value;

    friend bool operator==(fallible_value left, fallible_value right) {
        if (throw_on_equality) {
            throw std::runtime_error{"equality failed"};
        }
        return left.value == right.value;
    }
};

} // namespace test_scalar

template <> struct std::hash<test_scalar::fallible_value> {
    std::size_t operator()(test_scalar::fallible_value value) const {
        if (test_scalar::throw_on_hash) {
            throw std::runtime_error{"hash failed"};
        }
        return std::hash<bg::i64>{}(value.value);
    }
};

namespace bg {

template <> struct scalar_type_traits<test_scalar::fallible_value> {
    static const ScalarType &type() noexcept {
        class Type final : public ScalarType {
          public:
            bool is_resolved() const noexcept override { return true; }
            const bg::Type &resolve(const TypeMap &) const override { return *this; }
            bool supports_equality() const noexcept override { return true; }
            bool supports_hash() const noexcept override { return true; }
        };

        static const auto type = util::static_storage<Type>{};
        [[maybe_unused]] static const auto &registration = TypeRegistry::register_static(type.get());
        return type.get();
    }
};

} // namespace bg

TEST_CASE("scalar collections propagate hash and equality exceptions",
          "[value][scalar][collection][feature:scalar-collection]") {
    using Value = test_scalar::fallible_value;

    const auto set = bg::ScalarValue<bg::Set<Value>>{std::array{Value{1}}};
    const auto set_copy = bg::ScalarValue<bg::Set<Value>>{std::array{Value{1}}};
    const auto list = bg::ScalarValue<bg::List<Value>>{std::array{Value{1}}};
    const auto list_copy = bg::ScalarValue<bg::List<Value>>{std::array{Value{1}}};
    const auto dictionary = bg::ScalarValue<bg::Dict<Value, bg::i64>>{
        std::array{std::pair{Value{1}, bg::i64{10}}}};
    const auto dictionary_copy = bg::ScalarValue<bg::Dict<Value, bg::i64>>{
        std::array{std::pair{Value{1}, bg::i64{10}}}};

    const auto scalar = bg::ScalarValue<Value>{Value{1}};

    test_scalar::throw_on_hash = true;
    CHECK_THROWS_AS(scalar.hash(), std::runtime_error);
    CHECK_THROWS_AS(set.view().hash(), std::runtime_error);
    CHECK_THROWS_AS(list.view().hash(), std::runtime_error);
    CHECK_THROWS_AS(dictionary.view().contains(Value{1}), std::runtime_error);

    test_scalar::throw_on_hash = false;
    test_scalar::throw_on_equality = true;
    CHECK_THROWS_AS(set.view().contains(Value{1}), std::runtime_error);
    CHECK_THROWS_AS(set == set_copy, std::runtime_error);
    CHECK_THROWS_AS(list == list_copy, std::runtime_error);
    CHECK_THROWS_AS(dictionary.view().contains(Value{1}), std::runtime_error);
    CHECK_THROWS_AS(dictionary == dictionary_copy, std::runtime_error);

    test_scalar::throw_on_equality = false;
    CHECK(set.view().contains(Value{1}));
    CHECK(dictionary.view().contains(Value{1}));
    CHECK(set == set_copy);
    CHECK(list == list_copy);
    CHECK(dictionary == dictionary_copy);
}

TEST_CASE("time series set reclamation does not hash stored keys",
          "[value][tss][storage][feature:time-series-storage]") {
    using Value = test_scalar::fallible_value;

    auto resource = std::pmr::unsynchronized_pool_resource{};
    auto memory = bg::allocator{resource};
    auto storage = bg::details::time_series_set_storage{};
    const auto &key_runtime_type = bg::cpp_scalar_runtime_type<Value>();
    const auto key_layout = bg::util::runtime_item_layout{key_runtime_type.size(), key_runtime_type.alignment()};
    const auto key = bg::ScalarValue<Value>{Value{1}};
    auto item_count = 0;
    const auto time = bg::MIN_DT + bg::EvaluationTime::duration{1};

    storage.reset_change_tracking(time, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --item_count; });
    test_scalar::throw_on_hash = true;
    CHECK_THROWS_AS(storage.insert(key, key_runtime_type, key_layout, memory,
                                   [&](std::uint32_t) { ++item_count; },
                                   [&](std::uint32_t) noexcept { --item_count; }),
                    std::runtime_error);
    test_scalar::throw_on_hash = false;
    CHECK(storage.size() == 0);
    CHECK(item_count == 0);

    const auto inserted = storage.insert(key, key_runtime_type, key_layout, memory,
                                         [&](std::uint32_t) { ++item_count; },
                                         [&](std::uint32_t) noexcept { --item_count; });
    REQUIRE(inserted.change == bg::details::time_series_set_change::added);
    storage.reset_change_tracking(time + bg::EvaluationTime::duration{1}, key_runtime_type, key_layout, memory,
                                  [&](std::uint32_t) noexcept { --item_count; });
    CHECK(storage.erase(key, key_runtime_type, key_layout, memory,
                        [&](std::uint32_t) noexcept { --item_count; }).change ==
          bg::details::time_series_set_change::removed);

    test_scalar::throw_on_hash = true;
    CHECK_NOTHROW((storage.reset_change_tracking(time + bg::EvaluationTime::duration{2}, key_runtime_type, key_layout,
                                                 memory, [&](std::uint32_t) noexcept { --item_count; })));
    test_scalar::throw_on_hash = false;

    storage.release(key_runtime_type, key_layout, memory, [&](std::uint32_t) noexcept { --item_count; });
    CHECK(item_count == 0);
}

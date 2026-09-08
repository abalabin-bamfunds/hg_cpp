#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

TEST_CASE("time series dictionary annotations expose their key and item types",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    using IntDictionary = bg::TSD<int, bg::TS<float>>;

    STATIC_CHECK(std::same_as<IntDictionary::key_type, int>);
    STATIC_CHECK(std::same_as<IntDictionary::item_type, bg::TS<float>>);
    STATIC_CHECK(bg::details::is_time_series_dict<IntDictionary>);
}

TEST_CASE("resolved time series dictionary types are canonical",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    const auto &value = bg::TS<float>::type();
    const auto &dictionary = bg::TSD<int, bg::TS<float>>::type();

    CHECK(&dictionary == &bg::TSD<int, bg::TS<float>>::type());
    CHECK(&dictionary.key_type() == &bg::int_type());
    CHECK(&dictionary.value_type() == &value);
    CHECK(&dictionary.key_set_type() == &bg::TSS<int>::type());
    CHECK(&dictionary.item_type(0) == &dictionary.key_set_type());
    CHECK_THROWS_AS(dictionary.item_type(1), std::out_of_range);
}

TEST_CASE("generic time series dictionary types resolve their components",
          "[type_system][tsd][series:tsd][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved =
        bg::TSD<int, bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type();
    bg::TypeVariable<bg::ScalarType> key{bg::util::symbol{"K"}, {}, {&bg::supports_scalar_hash}};
    bg::TypeVariable<bg::TimeSeriesType> value{bg::util::symbol{"V"}};
    const bg::GenericTimeSeriesSetType key_set{key, bg::TimeSeriesGuarantee::final};
    const bg::GenericTimeSeriesDictionaryType generic{key, value, key_set, bg::TimeSeriesGuarantee::final};
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(resolved, type_map));
    CHECK(&generic.resolve(type_map) == &resolved);
}

TEST_CASE("dictionary item matching is reference transparent",
          "[type_system][tsd][series:tsd][reference][feature:type-system][binding:reference-to-value]") {
    const auto &item = bg::TS<float>::type();
    const auto &reference = bg::REF<bg::TS<float>>::type();
    const auto &key_set = bg::TSS<int>::type();
    const auto direct_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int_type(), item, key_set, bg::TimeSeriesGuarantee::ordinary};
    const auto reference_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int_type(), reference, key_set, bg::TimeSeriesGuarantee::ordinary};
    const auto &direct = bg::TSD<int, bg::TS<float>>::type();
    const auto &references = bg::TSD<int, bg::REF<bg::TS<float>>>::type();
    auto type_map = bg::TypeMap{};

    CHECK(direct_dictionary.match(references, type_map));
    type_map = bg::TypeMap{};
    CHECK(reference_dictionary.match(direct, type_map));
}

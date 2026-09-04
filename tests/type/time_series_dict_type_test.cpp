#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
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
    const auto &value = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &dictionary = bg::time_series_dictionary_type(bg::int_type(), value, bg::TimeSeriesGuarantee::ordinary);

    CHECK(&dictionary == &bg::time_series_dictionary_type(bg::int_type(), value, bg::TimeSeriesGuarantee::ordinary));
    CHECK(&dictionary.key_type() == &bg::int_type());
    CHECK(&dictionary.value_type() == &value);
    CHECK(&dictionary.key_set_type() == &bg::time_series_set_type(bg::int_type(), bg::TimeSeriesGuarantee::ordinary));
    CHECK(&dictionary.item_type(0) == &dictionary.key_set_type());
    CHECK_THROWS_AS(dictionary.item_type(1), std::out_of_range);
}

TEST_CASE("generic time series dictionary types resolve their components",
          "[type_system][tsd][series:tsd][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved_value = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::final);
    const auto &resolved =
        bg::time_series_dictionary_type(bg::int_type(), resolved_value, bg::TimeSeriesGuarantee::final);
    bg::TypeVariable<bg::ScalarType> key{bg::util::symbol{"K"}, {}, {&bg::supports_scalar_hash}};
    bg::TypeVariable<bg::TimeSeriesType> value{bg::util::symbol{"V"}};
    const bg::GenericTimeSeriesSetType key_set{key, bg::TimeSeriesGuarantee::final};
    const bg::GenericTimeSeriesDictionaryType generic{key, value, key_set, bg::TimeSeriesGuarantee::final};
    auto bindings = bg::TypeMap{};

    REQUIRE(generic.match(resolved, bindings));
    CHECK(&generic.resolve(bindings) == &resolved);
}

TEST_CASE("dictionary item matching is reference transparent",
          "[type_system][tsd][series:tsd][reference][feature:type-system][binding:reference-to-value]") {
    const auto &item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(item, bg::TimeSeriesGuarantee::ordinary);
    const auto &key_set = bg::time_series_set_type(bg::int_type(), bg::TimeSeriesGuarantee::ordinary);
    const auto direct_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int_type(), item, key_set, bg::TimeSeriesGuarantee::ordinary};
    const auto reference_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int_type(), reference, key_set, bg::TimeSeriesGuarantee::ordinary};
    const auto &direct = bg::time_series_dictionary_type(bg::int_type(), item, bg::TimeSeriesGuarantee::ordinary);
    const auto &references =
        bg::time_series_dictionary_type(bg::int_type(), reference, bg::TimeSeriesGuarantee::ordinary);
    auto bindings = bg::TypeMap{};

    CHECK(direct_dictionary.match(references, bindings));
    bindings = bg::TypeMap{};
    CHECK(reference_dictionary.match(direct, bindings));
}

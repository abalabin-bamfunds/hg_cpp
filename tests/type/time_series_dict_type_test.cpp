#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

TEST_CASE("time series dictionary annotations expose their key and item types",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    using IntDictionary = bg::TSD<bg::i64, bg::TS<float>>;

    STATIC_CHECK(std::same_as<IntDictionary::key_type, bg::i64>);
    STATIC_CHECK(std::same_as<IntDictionary::item_type, bg::TS<float>>);
    STATIC_CHECK(bg::details::is_time_series_dict<IntDictionary>);
}

TEST_CASE("time series dictionary declarations require canonical integer keys",
          "[feature:type-system][series:tsd][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(bg::details::supported_time_series<bg::TSD<int, bg::TS<bg::i64>>>);
    STATIC_CHECK(bg::details::supported_time_series<bg::TSD<bg::i64, bg::TS<bg::i64>>>);
}

TEST_CASE("resolved time series dictionary types are canonical",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    const auto &value = bg::TS<float>::type();
    const auto &dictionary = bg::TSD<bg::i64, bg::TS<float>>::type();

    CHECK(&dictionary == &bg::TSD<bg::i64, bg::TS<float>>::type());
    CHECK(&dictionary.key_type() == &bg::int64_type());
    CHECK(&dictionary.value_type() == &value);
    CHECK(&dictionary.key_set_type() == &bg::TSS<bg::i64>::type());
    CHECK(&dictionary.item_type(0) == &dictionary.key_set_type());
    CHECK_THROWS_AS(dictionary.item_type(1), std::out_of_range);
}

TEST_CASE("common dictionaries combine item guarantees and retain key types",
          "[feature:type-system][series:tsd][guarantee:ordinary][guarantee:final]") {
    const auto &final_dictionary =
        bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                bg::TimeSeriesGuarantee::final>::type();
    const auto &ordinary_dictionary = bg::TSD<bg::i64, bg::TS<bg::i64>>::type();
    const auto *common = final_dictionary.common_type(ordinary_dictionary);

    REQUIRE(common != nullptr);
    const auto &dictionary = bg::type_cast<bg::TimeSeriesDictionaryType>(*common);
    CHECK(&dictionary == &bg::TSD<bg::i64, bg::TS<bg::i64>>::type());
    CHECK(ordinary_dictionary.common_type(final_dictionary) == common);
    CHECK(final_dictionary.common_type(bg::TSD<bg::symbol, bg::TS<bg::i64>>::type()) == nullptr);
    CHECK(bg::TSD<bg::symbol, bg::TS<bg::i64>>::type().common_type(final_dictionary) == nullptr);
    CHECK(final_dictionary.common_type(bg::TSD<bg::i64, bg::TS<float>>::type()) == nullptr);
    CHECK(bg::TSD<bg::i64, bg::TS<float>>::type().common_type(final_dictionary) == nullptr);

    const auto &unresolved = bg::TSD<bg::i64, bg::TS<bg::SCALAR>>::type();
    const auto &resolved = bg::TSD<bg::i64, bg::TS<bg::i64>>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("common nested dictionary values combine guarantees recursively",
          "[feature:type-system][series:tsd][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &left = bg::TSD<bg::i64,
                               bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2,
                                       bg::TimeSeriesGuarantee::final>,
                               bg::TimeSeriesGuarantee::final>::type();
    const auto &right = bg::TSD<bg::i64,
                                bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, 2>>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &dictionary = bg::type_cast<bg::TimeSeriesDictionaryType>(*common);
    const auto &list = bg::type_cast<bg::TimeSeriesListType>(dictionary.value_type());
    CHECK(dictionary.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(list.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(list.item_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("dictionary declaration reference items keep a neutral guarantee",
          "[type_system][tsd][reference][series:tsd][feature:type-system][guarantee:ordinary]") {
    const auto &dictionary = bg::TSD<bg::i64, bg::REF<bg::TS<float>>>::type();

    CHECK(dictionary.value_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a dictionary guarantee strengthens an explicit item guarantee",
          "[type_system][tsd][series:tsd][feature:type-system][guarantee:ordinary][guarantee:final]") {
    const auto &dictionary =
        bg::TSD<bg::i64, bg::TS<float, bg::TimeSeriesGuarantee::ordinary>, bg::TimeSeriesGuarantee::final>::type();

    CHECK(dictionary.value_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("generic time series dictionary types resolve their components",
          "[type_system][tsd][series:tsd][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved =
        bg::TSD<bg::i64, bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type();
    using KEY = bg::ScalarVariable<"K", bg::Hashable>;
    using VALUE = bg::TimeSeriesVariable<"V">;
    const auto &generic = bg::TSD<KEY, VALUE, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(resolved, type_map));
    CHECK(&generic.resolve(type_map) == &resolved);
}

TEST_CASE("dictionary item matching is reference transparent",
          "[type_system][tsd][series:tsd][reference][feature:type-system][binding:reference-to-value]") {
    const auto &item = bg::TS<float>::type();
    const auto &reference = bg::REF<bg::TS<float>>::type();
    const auto &key_set = bg::TSS<bg::i64>::type();
    static const auto direct_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int64_type(), item, key_set, bg::TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &direct_dictionary_registration =
        bg::TypeRegistry::register_static(direct_dictionary);
    static const auto reference_dictionary =
        bg::GenericTimeSeriesDictionaryType{bg::int64_type(), reference, key_set, bg::TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &reference_dictionary_registration =
        bg::TypeRegistry::register_static(reference_dictionary);
    const auto &direct = bg::TSD<bg::i64, bg::TS<float>>::type();
    const auto &references = bg::TSD<bg::i64, bg::REF<bg::TS<float>>>::type();
    auto type_map = bg::TypeMap{};

    CHECK(direct_dictionary.match(references, type_map));
    type_map = bg::TypeMap{};
    CHECK(reference_dictionary.match(direct, type_map));
}

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
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>

using namespace bg;

TEST_CASE("time series dictionary annotations expose their key and item types",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    using IntDictionary = TSD<i64, TS<float>>;

    STATIC_CHECK(std::same_as<IntDictionary::key_type, i64>);
    STATIC_CHECK(std::same_as<IntDictionary::item_type, TS<float>>);
    STATIC_CHECK(details::is_time_series_dict<IntDictionary>);
}

TEST_CASE("time series dictionary declarations require canonical integer keys",
          "[feature:type-system][series:tsd][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(details::supported_time_series<TSD<int, TS<i64>>>);
    STATIC_CHECK(details::supported_time_series<TSD<i64, TS<i64>>>);
}

TEST_CASE("resolved time series dictionary types are canonical",
          "[type_system][tsd][series:tsd][feature:type-system]") {
    const auto &value = TS<float>::type();
    const auto &dictionary = TSD<i64, TS<float>>::type();

    CHECK(&dictionary == &TSD<i64, TS<float>>::type());
    CHECK(&dictionary.key_type() == &int64_type());
    CHECK(&dictionary.value_type() == &value);
    CHECK(&dictionary.key_set_type() == &TSS<i64>::type());
    CHECK(&dictionary.item_type(0) == &dictionary.key_set_type());
    CHECK_THROWS_AS(dictionary.item_type(1), std::out_of_range);
}

TEST_CASE("common dictionaries combine item guarantees and retain key types",
          "[feature:type-system][series:tsd][guarantee:ordinary][guarantee:final]") {
    const auto &final_dictionary = TSD<i64, TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type();
    const auto &ordinary_dictionary = TSD<i64, TS<i64>>::type();
    const auto *common = final_dictionary.common_type(ordinary_dictionary);

    REQUIRE(common != nullptr);
    const auto &dictionary = type_cast<TimeSeriesDictionaryType>(*common);
    CHECK(&dictionary == &TSD<i64, TS<i64>>::type());
    CHECK(ordinary_dictionary.common_type(final_dictionary) == common);
    CHECK(final_dictionary.common_type(TSD<symbol, TS<i64>>::type()) == nullptr);
    CHECK(TSD<symbol, TS<i64>>::type().common_type(final_dictionary) == nullptr);
    CHECK(final_dictionary.common_type(TSD<i64, TS<float>>::type()) == nullptr);
    CHECK(TSD<i64, TS<float>>::type().common_type(final_dictionary) == nullptr);

    const auto &unresolved = TSD<i64, TS<SCALAR>>::type();
    const auto &resolved = TSD<i64, TS<i64>>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("common nested dictionary values combine guarantees recursively",
          "[feature:type-system][series:tsd][guarantee:ordinary][guarantee:final][guarantee:const]") {
    const auto &left = TSD<i64, TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>,
                           TimeSeriesGuarantee::final>::type();
    const auto &right = TSD<i64, TSL<TS<i64, TimeSeriesGuarantee::const_>, 2>>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &dictionary = type_cast<TimeSeriesDictionaryType>(*common);
    const auto &list = type_cast<TimeSeriesListType>(dictionary.value_type());
    CHECK(dictionary.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(list.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(list.item_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("dictionary declaration reference items keep a neutral guarantee",
          "[type_system][tsd][reference][series:tsd][feature:type-system][guarantee:ordinary]") {
    const auto &dictionary = TSD<i64, REF<TS<float>>>::type();

    CHECK(dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a dictionary guarantee strengthens an explicit item guarantee",
          "[type_system][tsd][series:tsd][feature:type-system][guarantee:ordinary][guarantee:final]") {
    const auto &dictionary = TSD<i64, TS<float, TimeSeriesGuarantee::ordinary>, TimeSeriesGuarantee::final>::type();

    CHECK(dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("generic time series dictionary types resolve their components",
          "[type_system][tsd][series:tsd][feature:type-system][declaration:generic][guarantee:final]") {
    const auto &resolved = TSD<i64, TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type();
    using KEY = ScalarVariable<"K", Hashable>;
    using VALUE = TimeSeriesVariable<"V">;
    const auto &generic = TSD<KEY, VALUE, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(generic.match(resolved, type_map));
    CHECK(&generic.resolve(type_map) == &resolved);
}

TEST_CASE("dictionary item matching is reference transparent",
          "[type_system][tsd][series:tsd][reference][feature:type-system][binding:reference-to-value]") {
    const auto &item = TS<float>::type();
    const auto &reference = REF<TS<float>>::type();
    const auto &key_set = TSS<i64>::type();
    static const auto direct_dictionary = util::static_storage<GenericTimeSeriesDictionaryType>{
        int64_type(), item, key_set, TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &direct_dictionary_registration =
        TypeRegistry::register_static(direct_dictionary.get());
    static const auto reference_dictionary = util::static_storage<GenericTimeSeriesDictionaryType>{
        int64_type(), reference, key_set, TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &reference_dictionary_registration =
        TypeRegistry::register_static(reference_dictionary.get());
    const auto &direct = TSD<i64, TS<float>>::type();
    const auto &references = TSD<i64, REF<TS<float>>>::type();
    auto type_map = TypeMap{};

    CHECK(direct_dictionary.get().match(references, type_map));
    type_map = TypeMap{};
    CHECK(reference_dictionary.get().match(direct, type_map));
}

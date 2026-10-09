#include <bg/node.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>

namespace {

template <template <typename, auto...> typename Field> struct ReferenceGuaranteeBase {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> base_value;
};

template <template <typename, auto...> typename Field>
struct ReferenceGuaranteeDerived : bg::schema_extends<ReferenceGuaranteeBase, Field> {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> derived_value;
};

} // namespace

TEST_CASE("an omitted reference output guarantee is deduplicated",
          "[type][reference][feature:type-system][guarantee:deduplicated]") {
    CHECK(bg::REF<bg::TS<std::int64_t>>::type().guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);

    const auto explicit_ordinary =
        bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>::type().guarantee().value();
    CHECK(explicit_ordinary == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("an omitted reference input guarantee remains unconstrained",
          "[type][reference][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:deduplicated]") {
    const auto sink = bg::node([](bg::In<bg::REF<bg::TS<std::int64_t>>>) {});
    const auto &input = static_cast<const bg::TimeSeriesType &>(*sink.signature().parameter(0).type);

    CHECK_FALSE(input.guarantee().is_resolved());
}

TEST_CASE("time series reference types retain their target and guarantee",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:const]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &reference = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type();

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
    CHECK(&bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type() == &reference);
}

TEST_CASE("a generic reference accepts a reference or its target",
          "[type][reference][resolution][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:const][binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &reference = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::const_>::type();
    static const auto generic_target =
        bg::GenericTimeSeriesValueType{bg::int64_type(), bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};
    [[maybe_unused]] static const auto &generic_target_registration = bg::TypeRegistry::register_static(generic_target);
    static const auto generic_reference =
        bg::GenericTimeSeriesReferenceType{generic_target, bg::GuaranteeParameter{bg::TimeSeriesGuarantee::ordinary}};
    [[maybe_unused]] static const auto &generic_reference_registration =
        bg::TypeRegistry::register_static(generic_reference);

    auto type_map = bg::TypeMap{};
    CHECK(generic_reference.match(reference, type_map));

    type_map = bg::TypeMap{};
    CHECK(generic_reference.match(target, type_map));
}

TEST_CASE("generic time series references have process-owned type identity",
          "[type][reference][resolution][feature:type-system][declaration:generic]") {
    const auto &generic_reference = bg::generic_time_series_reference_type(
        bg::TS_T::type(), bg::TimeSeriesGuarantee::ordinary);
    CHECK(&generic_reference == &bg::generic_time_series_reference_type(
                                    bg::TS_T::type(), bg::TimeSeriesGuarantee::ordinary));

    const auto &reference = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};
    REQUIRE(generic_reference.match(reference, type_map));

    const auto &resolved = static_cast<const bg::TimeSeriesReferenceType &>(generic_reference.resolve(type_map));
    CHECK(&resolved.target_type() == &bg::TS<std::int64_t>::type());
    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("reference matching maps target and reference guarantees separately",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &reference =
        bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::deduplicated>::type();
    static const auto target_guarantee = bg::GuaranteeVariable{bg::util::symbol{"target guarantee"}};
    [[maybe_unused]] static const auto &target_guarantee_registration =
        bg::VariableRegistry::register_static(target_guarantee);
    static const auto reference_guarantee = bg::GuaranteeVariable{bg::util::symbol{"reference guarantee"}};
    [[maybe_unused]] static const auto &reference_guarantee_registration =
        bg::VariableRegistry::register_static(reference_guarantee);
    static const auto generic_target = bg::GenericTimeSeriesValueType{bg::int64_type(), target_guarantee};
    [[maybe_unused]] static const auto &generic_target_registration = bg::TypeRegistry::register_static(generic_target);
    static const auto generic_reference = bg::GenericTimeSeriesReferenceType{generic_target, reference_guarantee};
    [[maybe_unused]] static const auto &generic_reference_registration =
        bg::TypeRegistry::register_static(generic_reference);
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_reference.match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesReferenceType &>(generic_reference.resolve(type_map));

    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK(resolved.target_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("dereferenced matching enforces the effective guarantee minimum",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto minimum = bg::GuaranteeVariable{bg::util::symbol{"dereferenced minimum"},
                                                       bg::TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &minimum_registration = bg::VariableRegistry::register_static(minimum);
    static const auto parameter = bg::GenericTimeSeriesValueType{bg::int64_type(), minimum};
    [[maybe_unused]] static const auto &parameter_registration = bg::TypeRegistry::register_static(parameter);

    const auto &final_target = bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>::type();
    const auto &weak_reference =
        bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type();
    const auto &strong_reference =
        bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(parameter.match(weak_reference, type_map));

    type_map = bg::TypeMap{};
    REQUIRE(parameter.match(strong_reference, type_map));
    CHECK(&parameter.resolve(type_map) == &final_target);
}

TEST_CASE("dereferenced matching limits each target guarantee by the reference guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::deduplicated,
                                          bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::const_};
    constexpr auto expected_guarantees = std::array{
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary,
                   bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary,
                   bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::ordinary},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::deduplicated,
                   bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::final},
        std::array{bg::TimeSeriesGuarantee::ordinary, bg::TimeSeriesGuarantee::deduplicated,
                   bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::const_},
    };

    for (auto reference_index = std::size_t{}; reference_index < guarantees.size(); ++reference_index) {
        for (auto target_index = std::size_t{}; target_index < guarantees.size(); ++target_index) {
            const auto reference_guarantee = guarantees[reference_index];
            const auto target_guarantee = guarantees[target_index];
            const auto &target = bg::time_series_value_type(bg::int64_type(), target_guarantee);
            const auto &reference = bg::time_series_reference_type(target, reference_guarantee);
            auto type_map = bg::TypeMap{};
            CAPTURE(reference_guarantee, target_guarantee);

            REQUIRE(bg::TS_T::type().match(reference, type_map));

            const auto expected_guarantee = expected_guarantees[reference_index][target_index];
            const auto &expected = bg::time_series_value_type(bg::int64_type(), expected_guarantee);
            CHECK(&bg::TS_T::type().resolve(type_map) == &expected);
        }
    }
}

TEST_CASE("dereferenced guarantee variables resolve from the observed guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final]") {
    static const auto guarantee = bg::GuaranteeVariable{bg::util::symbol{"observed guarantee"}};
    [[maybe_unused]] static const auto &guarantee_registration = bg::VariableRegistry::register_static(guarantee);
    static const auto parameter = bg::GenericTimeSeriesValueType{bg::int64_type(), guarantee};
    [[maybe_unused]] static const auto &parameter_registration = bg::TypeRegistry::register_static(parameter);

    const auto &reference =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(reference, type_map));
    CHECK(parameter.resolve(type_map).is_time_series());
    CHECK(static_cast<const bg::TimeSeriesType &>(parameter.resolve(type_map)).guarantee().value() ==
          bg::TimeSeriesGuarantee::final);
}

TEST_CASE("an unresolved time series cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]") {
    CHECK_THROWS_AS(bg::TS_T::type().with_guarantee_limit(bg::TimeSeriesGuarantee::ordinary), bg::TypeResolutionError);
}

TEST_CASE("dereferenced matching rejects unresolved references",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[binding:reference-to-value]") {
    const auto &unresolved_target =
        bg::generic_time_series_reference_type(bg::TS_T::type(), bg::TimeSeriesGuarantee::ordinary);
    static const auto reference_guarantee = bg::GuaranteeVariable{bg::util::symbol{"unresolved reference guarantee"}};
    [[maybe_unused]] static const auto &reference_guarantee_registration =
        bg::VariableRegistry::register_static(reference_guarantee);
    static const auto unresolved_guarantee = bg::GenericTimeSeriesReferenceType{
        bg::TS<bg::i64>::type(), bg::GuaranteeParameter{reference_guarantee}};
    [[maybe_unused]] static const auto &unresolved_guarantee_registration =
        bg::TypeRegistry::register_static(unresolved_guarantee);
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(bg::TS_T::type().match(unresolved_target, type_map));
    CHECK_FALSE(bg::TS_T::type().match(unresolved_guarantee, type_map));

    CHECK_THROWS_AS(unresolved_target.dereferenced_type(), bg::TypeResolutionError);
    CHECK_THROWS_AS(unresolved_guarantee.dereferenced_type(), bg::TypeResolutionError);
}

TEST_CASE("an unresolved TSL cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:fixed-tsl]") {
    CHECK_THROWS_AS((bg::TSL<bg::TS_T, 2>::type().with_guarantee_limit(bg::TimeSeriesGuarantee::ordinary)),
                    bg::TypeResolutionError);
}

TEST_CASE("an unresolved TSD cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:tsd]") {
    CHECK_THROWS_AS((bg::TSD<bg::SCALAR, bg::TS_T>::type().with_guarantee_limit(bg::TimeSeriesGuarantee::ordinary)),
                    bg::TypeResolutionError);
}

TEST_CASE("an unresolved TSB cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:tsb]") {
    CHECK_THROWS_AS(bg::TSB<bg::SCHEMA>::type().with_guarantee_limit(bg::TimeSeriesGuarantee::ordinary),
                    bg::TypeResolutionError);
}

TEST_CASE("dereferenced type variables resolve to the observed time series type",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    const auto &reference =
        bg::REF<bg::TS<std::int64_t, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    CHECK(&bg::TS_T::type().resolve(type_map) == &bg::TS<std::int64_t>::type());
}

TEST_CASE("nested reference item matching combines its own guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    using VALUE = bg::TS<double, bg::guarantee_variable<"Item guarantee">>;
    using DICTIONARY = bg::TSD<std::int64_t, VALUE, bg::guarantee_variable<"Dictionary guarantee">>;
    using TARGET_ITEM = bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>;
    using ARGUMENT = bg::TSD<std::int64_t, TARGET_ITEM>;
    const auto &parameter = DICTIONARY::type();
    const auto &argument = ARGUMENT::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) ==
          &bg::TSD<std::int64_t, bg::TS<double>>::type());
}

TEST_CASE("dereferenced collection matching limits guarantees recursively",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    using TARGET =
        bg::TSD<bg::i64, bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, 2, bg::TimeSeriesGuarantee::final>>;
    using EXPECTED =
        bg::TSD<bg::i64, bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2, bg::TimeSeriesGuarantee::final>>;
    const auto &reference = bg::REF<TARGET, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    CHECK(&bg::TS_T::type().resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("a mutable reference limits stronger contained guarantees even when its root is ordinary",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final]") {
    using TARGET = bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>;
    using EXPECTED = bg::TSD<bg::i64, bg::TS<bg::i64>>;
    const auto &reference = bg::REF<TARGET, bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesDictionaryType &>(bg::TS_T::type().resolve(type_map));

    CHECK(&resolved == &EXPECTED::type());
    CHECK(resolved.key_set_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a const limit leaves every collection guarantee unchanged",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:const]") {
    using TARGET = bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                           bg::TimeSeriesGuarantee::const_>;
    const auto &target = TARGET::type();

    CHECK(&target.with_guarantee_limit(bg::TimeSeriesGuarantee::const_) == &target);
}

TEST_CASE("a reference guarantee also limits the dictionary key-set guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:final][guarantee:const]") {
    using TARGET = bg::TSD<bg::i64, bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>;
    using EXPECTED = bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                             bg::TimeSeriesGuarantee::final>;
    const auto &reference = bg::REF<TARGET, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesDictionaryType &>(bg::TS_T::type().resolve(type_map));

    CHECK(&resolved == &EXPECTED::type());
    CHECK(resolved.key_set_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("limiting bundle guarantees preserves named schema identity",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][schema:named][guarantee:final][guarantee:const]") {
    const auto &target = bg::TSB<ReferenceGuaranteeDerived>::type();
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::final);
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesBundleType &>(bg::TS_T::type().resolve(type_map));

    CHECK(resolved.schema().identity() == target.schema().identity());
    REQUIRE(resolved.schema().base() != nullptr);
    REQUIRE(target.schema().base() != nullptr);
    CHECK(resolved.schema().base()->identity() == target.schema().base()->identity());
    CHECK(resolved.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(resolved.item_type(1).guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("limiting a collection caps an explicit reference but preserves its target",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:final][guarantee:const][binding:reference-to-reference]") {
    using ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::const_>;
    using TARGET = bg::TSD<bg::i64, ITEM>;
    const auto &reference = bg::REF<TARGET, bg::TimeSeriesGuarantee::final>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesDictionaryType &>(bg::TS_T::type().resolve(type_map));
    const auto &item_reference = static_cast<const bg::TimeSeriesReferenceType &>(resolved.value_type());

    CHECK(item_reference.guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(&item_reference.target_type() == &bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type());
}

TEST_CASE("limiting a set reference caps its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tss][guarantee:final][guarantee:const]") {
    const auto &target = bg::time_series_set_type(bg::int64_type(), bg::TimeSeriesGuarantee::const_);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::final);
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesSetType &>(bg::TS_T::type().resolve(type_map));

    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("limiting an unnamed bundle recursively limits its fields",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][guarantee:final][guarantee:const]") {
    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"first"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()},
        bg::SchemaField{bg::util::symbol{"second"}, &bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()},
    };
    const auto &target = bg::TimeSeriesBundleType::from_fields(fields, bg::TimeSeriesGuarantee::ordinary);
    const auto &reference = bg::time_series_reference_type(target, bg::TimeSeriesGuarantee::final);
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesBundleType &>(bg::TS_T::type().resolve(type_map));

    CHECK_FALSE(resolved.schema().named());
    CHECK(resolved.item_type(0).guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(resolved.item_type(1).guarantee().value() == bg::TimeSeriesGuarantee::final);
}

TEST_CASE("reference matching enforces the reference guarantee",
          "[type][reference][resolution][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &required = bg::REF<bg::TS<std::int64_t>, bg::TimeSeriesGuarantee::final>::type();
    const auto &deduplicated = bg::REF<bg::TS<std::int64_t>>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(required.match(deduplicated, type_map));

    type_map = bg::TypeMap{};
    CHECK(required.match(target, type_map));
}

TEST_CASE("a reference cannot target another reference", "[type][reference][feature:type-system]") {
    CHECK_THROWS_AS((bg::REF<bg::REF<bg::TS<std::int64_t>>>::type()), std::invalid_argument);
}

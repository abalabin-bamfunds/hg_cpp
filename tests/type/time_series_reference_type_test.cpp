#include <bg/node.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
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

inline constexpr auto G1 = bg::guarantee_variable<"G1">;
inline constexpr auto G2 = bg::guarantee_variable<"G2">;

bool has_ordinary_guarantee(const bg::TimeSeriesType &type) noexcept {
    return type.guarantee().value() == bg::TimeSeriesGuarantee::ordinary;
}

bool has_final_guarantee(const bg::TimeSeriesType &type) noexcept {
    return type.guarantee().value() == bg::TimeSeriesGuarantee::final;
}

class ExtensionMatcherTimeSeriesType final : public bg::TimeSeriesType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }

    inline static const bg::TimeSeriesType *observed_type{};

  protected:
    bool do_match(const bg::Type &supplied, bg::TypeMap &) const override {
        observed_type = bg::type_cast<bg::TimeSeriesType>(&supplied);
        return observed_type != nullptr;
    }
};

template <template <typename, auto...> typename Field> struct ReferenceGuaranteeBase {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> base_value;
};

template <template <typename, auto...> typename Field>
struct ReferenceGuaranteeDerived : bg::schema_extends<ReferenceGuaranteeBase, Field> {
    Field<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>> derived_value;
};

template <template <typename, auto...> typename Field> struct ReferenceFreeBase {
    Field<bg::REF<bg::TS<bg::i64>>> base_value;
};

template <template <typename, auto...> typename Field>
struct ReferenceFreeDerived : bg::schema_extends<ReferenceFreeBase, Field> {
    Field<bg::TSL<bg::REF<bg::TS<float>>, 2>> derived_value;
};

class ReferenceContainingExtensionType final : public bg::TimeSeriesType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "ReferenceContainingExtension"; }
    bool contains_reference() const override { return true; }
};

} // namespace

TEST_CASE("reference declarations are neutral and outputs default to deduplicated",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:deduplicated]") {
    const auto &reference = bg::REF<bg::TS<bg::i64>>::type();
    CHECK(reference.is_resolved());
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(&reference == &bg::REF<bg::TS<bg::i64>>::type());

    const auto explicit_ordinary =
        bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::ordinary>::type().guarantee().value();
    CHECK(explicit_ordinary == bg::TimeSeriesGuarantee::ordinary);

    const auto producer = bg::node([](bg::Out<bg::REF<bg::TS<bg::i64>>>) {});
    CHECK(producer.signature().output()->guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);

    using Dictionary = bg::TSD<bg::i64, bg::REF<bg::TS<bg::i64>>>;
    const auto &dictionary = Dictionary::type();
    CHECK(dictionary.is_resolved());
    CHECK(dictionary.value_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(&dictionary == &Dictionary::type());

    const auto dictionary_producer = bg::node(
        [](bg::Out<bg::TSD<bg::i64, bg::REF<bg::TS<bg::i64>>>>) {});
    const auto &output = static_cast<const bg::TimeSeriesDictionaryType &>(*dictionary_producer.signature().output());
    CHECK(output.value_type().guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
}

TEST_CASE("an omitted reference input guarantee remains unconstrained",
          "[type][reference][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:deduplicated]") {
    const auto sink = bg::node([](bg::In<bg::REF<bg::TS<bg::i64>>>) {});
    const auto &input = static_cast<const bg::TimeSeriesType &>(*sink.signature().parameter(0).type);

    CHECK_FALSE(input.guarantee().is_resolved());
}

TEST_CASE("time series reference types retain their target and guarantee",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:const]") {
    const auto &target = bg::TS<bg::i64>::type();
    const auto &reference = bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>::type();

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::const_);
    CHECK(&bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>::type() == &reference);
}

TEST_CASE("reference representation query checks an unbounded collection item type",
          "[type][reference][feature:type-system][series:fixed-tsl]") {
    const auto &reference_items = bg::time_series_list_type(
        bg::REF<bg::TS<bg::i64>>::type(), std::dynamic_extent, bg::TimeSeriesGuarantee::ordinary);
    const auto &value_items = bg::time_series_list_type(
        bg::TS<bg::i64>::type(), std::dynamic_extent, bg::TimeSeriesGuarantee::ordinary);

    CHECK(reference_items.requires_reference_representation_for(value_items));
}

TEST_CASE("common references combine target and reference guarantees separately",
          "[feature:type-system][series:reference][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-reference]") {
    const auto &left = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>,
                               bg::TimeSeriesGuarantee::final>::type();
    const auto &right = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                bg::TimeSeriesGuarantee::deduplicated>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &reference = bg::type_cast<bg::TimeSeriesReferenceType>(*common);
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::deduplicated);
    CHECK(reference.target_type().guarantee().value() == bg::TimeSeriesGuarantee::final);
    CHECK(right.common_type(left) == common);
    CHECK(left.common_type(bg::TS<bg::i64>::type()) == nullptr);
    CHECK(bg::TS<bg::i64>::type().common_type(left) == nullptr);
    CHECK(bg::REF<bg::TS<bg::i64>>::type().common_type(bg::REF<bg::TS<float>>::type()) == nullptr);
    CHECK(bg::REF<bg::TS<float>>::type().common_type(bg::REF<bg::TS<bg::i64>>::type()) == nullptr);

    const auto &unresolved = bg::REF<bg::TS<bg::SCALAR>>::type();
    const auto &resolved = bg::REF<bg::TS<bg::i64>>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("reference representation query detects fixed-list item changes",
          "[feature:type-system][series:fixed-tsl]") {
    const auto &reference_list = bg::TSL<bg::REF<bg::TS<bg::i64>>, 2>::type();
    const auto &value_list = bg::TSL<bg::TS<bg::i64>, 2>::type();

    CHECK(reference_list.requires_reference_representation_for(value_list));
}

TEST_CASE("reference representation query detects bundle field changes",
          "[feature:type-system][series:tsb]") {
    const auto fields = std::array{
        bg::SchemaField{bg::util::symbol{"first"}, &bg::REF<bg::TS<bg::i64>>::type()},
        bg::SchemaField{bg::util::symbol{"second"}, &bg::REF<bg::TS<bg::i64>>::type()},
    };
    const auto &reference_bundle =
        bg::TimeSeriesBundleType::from_fields(fields, bg::TimeSeriesGuarantee::ordinary);
    const auto value_fields = std::array{
        bg::SchemaField{bg::util::symbol{"first"}, &bg::TS<bg::i64>::type()},
        bg::SchemaField{bg::util::symbol{"second"}, &bg::TS<bg::i64>::type()},
    };
    const auto &value_bundle = bg::TimeSeriesBundleType::from_fields(value_fields, bg::TimeSeriesGuarantee::ordinary);

    CHECK(reference_bundle.requires_reference_representation_for(value_bundle));
}

TEST_CASE("reference representation query detects dictionary value changes",
          "[feature:type-system][series:tsd]") {
    const auto &reference_dictionary = bg::TSD<bg::i64, bg::REF<bg::TS<bg::i64>>>::type();
    const auto &value_dictionary = bg::TSD<bg::i64, bg::TS<bg::i64>>::type();

    CHECK(reference_dictionary.requires_reference_representation_for(value_dictionary));
}

TEST_CASE("reference representation query recurses through dictionary reference values",
          "[feature:type-system][series:tsd][series:fixed-tsl]") {
    const auto &reference_dictionary =
        bg::TSD<bg::i64, bg::REF<bg::TSL<bg::REF<bg::TS<bg::i64>>, 2>>>::type();
    const auto &value_dictionary = bg::TSD<bg::i64, bg::REF<bg::TSL<bg::TS<bg::i64>, 2>>>::type();

    CHECK(reference_dictionary.requires_reference_representation_for(value_dictionary));
}

TEST_CASE("common reference targets combine nested collection guarantees",
          "[feature:type-system][series:reference][series:tsd][guarantee:ordinary][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &left = bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                      bg::TimeSeriesGuarantee::final>>::type();
    const auto &right = bg::REF<bg::TSD<bg::i64, bg::TS<bg::i64>>>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &reference = bg::type_cast<bg::TimeSeriesReferenceType>(*common);
    const auto &dictionary = bg::type_cast<bg::TimeSeriesDictionaryType>(reference.target_type());
    CHECK(reference.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(dictionary.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
    CHECK(dictionary.value_type().guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a generic reference accepts a reference or its target",
          "[type][reference][resolution][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:const][binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = bg::TS<bg::i64>::type();
    const auto &reference = bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>::type();
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

    const auto &reference = bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};
    REQUIRE(generic_reference.match(reference, type_map));

    const auto &resolved = static_cast<const bg::TimeSeriesReferenceType &>(generic_reference.resolve(type_map));
    CHECK(&resolved.target_type() == &bg::TS<bg::i64>::type());
    CHECK(resolved.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("reference matching maps target and reference guarantees separately",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &reference =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::deduplicated>::type();
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

    const auto &final_target = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    const auto &weak_reference =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type();
    const auto &strong_reference =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>::type();
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

TEST_CASE("reference limits preserve the existing extension matcher path",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto extension_type = ExtensionMatcherTimeSeriesType{};
    [[maybe_unused]] static const auto &extension_type_registration =
        bg::TypeRegistry::register_static(extension_type);
    const auto &final_target = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    const auto &supplied = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                   bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};
    ExtensionMatcherTimeSeriesType::observed_type = nullptr;

    REQUIRE(extension_type.match(final_target, type_map));
    CHECK(ExtensionMatcherTimeSeriesType::observed_type == &final_target);

    type_map = bg::TypeMap{};
    ExtensionMatcherTimeSeriesType::observed_type = nullptr;
    REQUIRE(extension_type.match(supplied, type_map));
    CHECK(ExtensionMatcherTimeSeriesType::observed_type == &bg::TS<bg::i64>::type());
}

TEST_CASE("time-series type-variable constraints inspect the observed guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto ordinary_variable = bg::TypeVariableType<bg::TimeSeriesType>{
        bg::util::symbol{"ordinary observed series"}, {}, {&has_ordinary_guarantee}};
    [[maybe_unused]] static const auto &ordinary_variable_registration =
        bg::TypeRegistry::register_static(ordinary_variable);
    static const auto final_variable = bg::TypeVariableType<bg::TimeSeriesType>{
        bg::util::symbol{"final observed series"}, {}, {&has_final_guarantee}};
    [[maybe_unused]] static const auto &final_variable_registration =
        bg::TypeRegistry::register_static(final_variable);
    const auto &supplied = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                   bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(ordinary_variable.match(supplied, type_map));
    CHECK(&ordinary_variable.resolve(type_map) == &bg::TS<bg::i64>::type());

    type_map = bg::TypeMap{};
    CHECK_FALSE(final_variable.match(supplied, type_map));
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
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(bg::TS_T::type().match(reference, type_map));
    CHECK(&bg::TS_T::type().resolve(type_map) == &bg::TS<bg::i64>::type());
}

TEST_CASE("nested reference item matching combines its own guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    using VALUE = bg::TS<double, bg::guarantee_variable<"Item guarantee">>;
    using DICTIONARY = bg::TSD<bg::i64, VALUE, bg::guarantee_variable<"Dictionary guarantee">>;
    using TARGET_ITEM = bg::REF<bg::TS<double, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>;
    using ARGUMENT = bg::TSD<bg::i64, TARGET_ITEM>;
    const auto &parameter = DICTIONARY::type();
    const auto &argument = ARGUMENT::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) ==
          &bg::TSD<bg::i64, bg::TS<double>>::type());
}

TEST_CASE("a reference limit preserves nested reference targets and caps their guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][binding:reference-to-reference]") {
    using ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                         bg::guarantee_variable<"G">>;
    using PARAMETER = bg::TSD<bg::i64, ITEM>;
    using SUPPLIED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = bg::TSD<bg::i64, SUPPLIED_ITEM>;
    using SUPPLIED = bg::REF<SUPPLIED_TARGET, bg::TimeSeriesGuarantee::ordinary>;
    using EXPECTED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                  bg::TimeSeriesGuarantee::ordinary>;
    using EXPECTED = bg::TSD<bg::i64, EXPECTED_ITEM>;
    const auto &parameter = PARAMETER::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(SUPPLIED::type(), type_map));
    CHECK(&parameter.resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("an outer reference limit rejects stronger nested reference guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][guarantee:const]") {
    using ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>;
    using PARAMETER = bg::TSD<bg::i64, ITEM>;
    using SUPPLIED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = bg::TSD<bg::i64, SUPPLIED_ITEM>;
    using SUPPLIED = bg::REF<SUPPLIED_TARGET, bg::TimeSeriesGuarantee::ordinary>;
    auto type_map = bg::TypeMap{};
    using CONST_PARAMETER = bg::TSD<
        bg::i64, bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>>;

    CHECK_FALSE(PARAMETER::type().match(SUPPLIED::type(), type_map));
    CHECK_FALSE(CONST_PARAMETER::type().match(SUPPLIED::type(), type_map));
}

TEST_CASE("an outer reference limit reaches TSL reference items",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:fixed-tsl][guarantee:ordinary][guarantee:final]") {
    using ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::guarantee_variable<"G">>;
    using PARAMETER = bg::TSL<ITEM, 2>;
    using SUPPLIED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = bg::TSL<SUPPLIED_ITEM, 2>;
    using SUPPLIED = bg::REF<SUPPLIED_TARGET, bg::TimeSeriesGuarantee::ordinary>;
    using EXPECTED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>,
                                  bg::TimeSeriesGuarantee::ordinary>;
    using EXPECTED = bg::TSL<EXPECTED_ITEM, 2>;
    const auto &parameter = PARAMETER::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(SUPPLIED::type(), type_map));
    CHECK(&parameter.resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("an outer reference limit reaches TSB reference fields",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][guarantee:ordinary][guarantee:final]") {
    const auto &field_target = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    static const auto field_guarantee = bg::GuaranteeVariable{bg::util::symbol{"bundle field guarantee"}};
    [[maybe_unused]] static const auto &field_guarantee_registration =
        bg::VariableRegistry::register_static(field_guarantee);
    static const auto generic_field = bg::GenericTimeSeriesReferenceType{
        field_target, bg::GuaranteeParameter{field_guarantee}};
    [[maybe_unused]] static const auto &generic_field_registration = bg::TypeRegistry::register_static(generic_field);
    const auto parameter_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &generic_field}};
    const auto &parameter_schema = bg::anonymous_schema(parameter_fields);
    static const auto parameter = bg::GenericTimeSeriesBundleType{
        parameter_schema, bg::TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &parameter_registration = bg::TypeRegistry::register_static(parameter);

    const auto &supplied_field =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>::type();
    const auto supplied_fields = std::array{bg::SchemaField{bg::util::symbol{"value"}, &supplied_field}};
    const auto &supplied_target = bg::TimeSeriesBundleType::from_fields(supplied_fields);
    const auto &supplied = bg::time_series_reference_type(supplied_target, bg::TimeSeriesGuarantee::ordinary);
    auto type_map = bg::TypeMap{};

    REQUIRE(parameter.match(supplied, type_map));
    const auto &resolved = static_cast<const bg::TimeSeriesBundleType &>(parameter.resolve(type_map));
    const auto &resolved_field = bg::type_cast<bg::TimeSeriesReferenceType>(resolved.item_type(0));
    CHECK(&resolved_field.target_type() == &field_target);
    CHECK(resolved_field.guarantee().value() == bg::TimeSeriesGuarantee::ordinary);
}

TEST_CASE("top-level reference union alternatives preserve their guarantees and limited target mapping",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-reference]") {
    using GENERIC_REFERENCE = bg::REF<bg::TS<bg::i64, G2>, G1>;
    using OMITTED_REFERENCE = bg::REF<bg::TS<bg::i64>>;
    using ORDINARY_REFERENCE =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>, bg::TimeSeriesGuarantee::ordinary>;
    using DEDUPLICATED_REFERENCE =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::deduplicated>, bg::TimeSeriesGuarantee::deduplicated>;
    using FINAL_REFERENCE =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::final>;
    using CONST_REFERENCE =
        bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>, bg::TimeSeriesGuarantee::const_>;
    using REFERENCE_UNION = bg::TypeVariable<"REFERENCE_UNION", GENERIC_REFERENCE, OMITTED_REFERENCE,
                                              ORDINARY_REFERENCE, DEDUPLICATED_REFERENCE, FINAL_REFERENCE,
                                              CONST_REFERENCE>;

    const auto &union_type = REFERENCE_UNION::type();
    const auto verify_match = [&](const bg::TimeSeriesType &supplied, bg::TimeSeriesGuarantee expected_root,
                                  bg::TimeSeriesGuarantee expected_target,
                                  const bg::TimeSeriesType &expected_mapping) {
        auto type_map = bg::TypeMap{};

        REQUIRE(union_type.match(supplied, type_map));
        const auto *root = std::get_if<bg::TimeSeriesGuarantee>(
            type_map.find(bg::details::guarantee_variable_instance<G1>()));
        const auto *target = std::get_if<bg::TimeSeriesGuarantee>(
            type_map.find(bg::details::guarantee_variable_instance<G2>()));
        REQUIRE(root != nullptr);
        REQUIRE(target != nullptr);
        CHECK(*root == expected_root);
        CHECK(*target == expected_target);
        CHECK(&union_type.resolve(type_map) == &expected_mapping);
    };

    verify_match(OMITTED_REFERENCE::type(), bg::TimeSeriesGuarantee::ordinary,
                 bg::TimeSeriesGuarantee::ordinary, bg::TS<bg::i64>::type());
    verify_match(ORDINARY_REFERENCE::type(), bg::TimeSeriesGuarantee::ordinary,
                 bg::TimeSeriesGuarantee::ordinary, bg::TS<bg::i64>::type());
    verify_match(DEDUPLICATED_REFERENCE::type(), bg::TimeSeriesGuarantee::deduplicated,
                 bg::TimeSeriesGuarantee::deduplicated,
                 bg::TS<bg::i64>::type());
    verify_match(FINAL_REFERENCE::type(), bg::TimeSeriesGuarantee::final, bg::TimeSeriesGuarantee::final,
                 bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    verify_match(CONST_REFERENCE::type(), bg::TimeSeriesGuarantee::const_, bg::TimeSeriesGuarantee::const_,
                 bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type());

    auto rejected_type_map = bg::TypeMap{};
    const auto &rejected = bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>,
                                   bg::TimeSeriesGuarantee::deduplicated>::type();
    CHECK_FALSE(union_type.match(rejected, rejected_type_map));
    CHECK(rejected_type_map.find(bg::details::guarantee_variable_instance<G1>()) == nullptr);
    CHECK(rejected_type_map.find(bg::details::guarantee_variable_instance<G2>()) == nullptr);
}

TEST_CASE("singleton reference unions enforce their explicit guarantees",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const][binding:reference-to-reference]") {
    using DEDUPLICATED = bg::TypeVariable<
        "DEDUPLICATED", bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::deduplicated>>;
    using FINAL = bg::TypeVariable<"FINAL", bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::final>>;
    using CONST = bg::TypeVariable<"CONST", bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>>;

    auto type_map = bg::TypeMap{};
    CHECK(DEDUPLICATED::type().match(
        bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::deduplicated>::type(), type_map));
    type_map = bg::TypeMap{};
    CHECK_FALSE(DEDUPLICATED::type().match(
        bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::ordinary>::type(), type_map));

    type_map = bg::TypeMap{};
    CHECK(FINAL::type().match(bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::final>::type(), type_map));
    type_map = bg::TypeMap{};
    CHECK_FALSE(FINAL::type().match(
        bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::deduplicated>::type(), type_map));

    type_map = bg::TypeMap{};
    CHECK(CONST::type().match(bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::const_>::type(), type_map));
    type_map = bg::TypeMap{};
    CHECK_FALSE(CONST::type().match(
        bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::final>::type(), type_map));
}

TEST_CASE("union alternatives match original nested reference shapes with their limit",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][binding:reference-to-reference]") {
    using ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::ordinary>;
    using DICTIONARY = bg::TSD<bg::i64, ITEM>;
    using ALTERNATIVE = bg::TypeVariable<"DICTIONARY", DICTIONARY>;
    using SUPPLIED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = bg::TSD<bg::i64, SUPPLIED_ITEM>;
    using SUPPLIED = bg::REF<SUPPLIED_TARGET, bg::TimeSeriesGuarantee::ordinary>;
    const auto &variable = ALTERNATIVE::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(variable.match(SUPPLIED::type(), type_map));
    CHECK(&variable.resolve(type_map) == &DICTIONARY::type());
}

TEST_CASE("failed nested reference matching rolls back type-variable mappings",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd]") {
    using KEY = bg::ScalarVariable<"KEY", bg::Hashable>;
    using ITEM = bg::REF<bg::TS<float, bg::TimeSeriesGuarantee::final>, bg::guarantee_variable<"G">>;
    using PARAMETER = bg::TSD<KEY, ITEM>;
    using SUPPLIED_ITEM = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, bg::TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = bg::TSD<bg::i64, SUPPLIED_ITEM>;
    using SUPPLIED = bg::REF<SUPPLIED_TARGET, bg::TimeSeriesGuarantee::ordinary>;
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(PARAMETER::type().match(SUPPLIED::type(), type_map));
    CHECK(type_map.find(KEY::type()) == nullptr);
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
    const auto &target = bg::TS<bg::i64>::type();
    const auto &required = bg::REF<bg::TS<bg::i64>, bg::TimeSeriesGuarantee::final>::type();
    const auto &deduplicated = bg::REF<bg::TS<bg::i64>>::type();
    auto type_map = bg::TypeMap{};

    CHECK_FALSE(required.match(deduplicated, type_map));

    type_map = bg::TypeMap{};
    CHECK(required.match(target, type_map));
}

TEST_CASE("a reference cannot target another reference", "[type][reference][feature:type-system]") {
    CHECK_THROWS_AS((bg::REF<bg::REF<bg::TS<bg::i64>>>::type()), std::invalid_argument);
}

TEST_CASE("reference-free types remove references recursively through collections",
          "[type][reference][feature:type-system][series:tsd][series:tsl]") {
    using SOURCE = bg::TSD<bg::i64, bg::TSL<bg::REF<bg::TS<bg::i64>>, 2>>;
    using EXPECTED = bg::TSD<bg::i64, bg::TSL<bg::TS<bg::i64>, 2>>;

    CHECK(&SOURCE::type().reference_free_type() == &EXPECTED::type());
    CHECK(&bg::TS<bg::i64>::type().reference_free_type() == &bg::TS<bg::i64>::type());
    CHECK_THROWS_AS(bg::TS<bg::SCALAR>::type().reference_free_type(), bg::TypeResolutionError);

    const auto extension_type = ReferenceContainingExtensionType{};
    CHECK_THROWS_AS(extension_type.reference_free_type(), std::invalid_argument);
}

TEST_CASE("reference-free TSB types preserve their named schema and base",
          "[type][reference][feature:type-system][series:tsb][schema:named]") {
    const auto &source = bg::TSB<ReferenceFreeDerived>::type();
    const auto &result = static_cast<const bg::TimeSeriesBundleType &>(source.reference_free_type());

    CHECK(result.schema().name() == source.schema().name());
    CHECK(result.schema().identity() == source.schema().identity());
    REQUIRE(result.schema().base() != nullptr);
    REQUIRE(source.schema().base() != nullptr);
    CHECK(result.schema().base()->identity() == source.schema().base()->identity());
    CHECK(&result.item_type(0) == &bg::TS<bg::i64>::type());
    const auto &items = static_cast<const bg::TimeSeriesListType &>(result.item_type(1));
    CHECK(&items.item_type() == &bg::TS<float>::type());
}

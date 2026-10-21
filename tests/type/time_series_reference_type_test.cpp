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
#include <bg/util/static_storage.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>
#include <vector>

using namespace bg;

namespace {} // namespace

TEST_CASE("reference declarations are neutral and outputs default to deduplicated",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:deduplicated]") {
    const auto &reference = REF<TS<i64>>::type();
    CHECK(reference.is_resolved());
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(&reference == &REF<TS<i64>>::type());

    const auto explicit_ordinary = REF<TS<i64>, TimeSeriesGuarantee::ordinary>::type().guarantee().value();
    CHECK(explicit_ordinary == TimeSeriesGuarantee::ordinary);

    const auto producer = node([](Out<REF<TS<i64>>>) {});
    CHECK(producer.signature().output()->guarantee().value() == TimeSeriesGuarantee::deduplicated);

    using Dictionary = TSD<i64, REF<TS<i64>>>;
    const auto &dictionary = Dictionary::type();
    CHECK(dictionary.is_resolved());
    CHECK(dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(&dictionary == &Dictionary::type());

    const auto dictionary_producer = node([](Out<TSD<i64, REF<TS<i64>>>>) {});
    const auto &output = static_cast<const TimeSeriesDictionaryType &>(*dictionary_producer.signature().output());
    CHECK(output.value_type().guarantee().value() == TimeSeriesGuarantee::deduplicated);
}

TEST_CASE("an omitted reference input guarantee remains unconstrained",
          "[type][reference][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:deduplicated]") {
    const auto sink = node([](In<REF<TS<i64>>>) {});
    const auto &input = static_cast<const TimeSeriesType &>(*sink.signature().parameter(0).type);

    CHECK_FALSE(input.guarantee().is_resolved());
}

TEST_CASE("time series reference types retain their target and guarantee",
          "[type][reference][feature:type-system][guarantee:ordinary][guarantee:const]") {
    const auto &target = TS<i64>::type();
    const auto &reference = REF<TS<i64>, TimeSeriesGuarantee::const_>::type();

    CHECK(&reference.target_type() == &target);
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::const_);
    CHECK(&REF<TS<i64>, TimeSeriesGuarantee::const_>::type() == &reference);
}

TEST_CASE("reference representation query checks an unbounded collection item type",
          "[type][reference][feature:type-system][series:fixed-tsl]") {
    const auto &reference_items =
        time_series_list_type(REF<TS<i64>>::type(), std::dynamic_extent, TimeSeriesGuarantee::ordinary);
    const auto &value_items =
        time_series_list_type(TS<i64>::type(), std::dynamic_extent, TimeSeriesGuarantee::ordinary);

    CHECK(reference_items.requires_reference_representation_for(value_items));
}

TEST_CASE("common references combine target and reference guarantees separately",
          "[feature:type-system][series:reference][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-reference]") {
    const auto &left = REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::final>::type();
    const auto &right = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &reference = type_cast<TimeSeriesReferenceType>(*common);
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(reference.target_type().guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(right.common_type(left) == common);
    CHECK(left.common_type(TS<i64>::type()) == nullptr);
    CHECK(TS<i64>::type().common_type(left) == nullptr);
    CHECK(REF<TS<i64>>::type().common_type(REF<TS<float>>::type()) == nullptr);
    CHECK(REF<TS<float>>::type().common_type(REF<TS<i64>>::type()) == nullptr);

    const auto &unresolved = REF<TS<SCALAR>>::type();
    const auto &resolved = REF<TS<i64>>::type();
    CHECK(unresolved.common_type(resolved) == nullptr);
    CHECK(resolved.common_type(unresolved) == nullptr);
}

TEST_CASE("reference representation query detects fixed-list item changes",
          "[feature:type-system][series:fixed-tsl]") {
    const auto &reference_list = TSL<REF<TS<i64>>, 2>::type();
    const auto &value_list = TSL<TS<i64>, 2>::type();

    CHECK(reference_list.requires_reference_representation_for(value_list));
}

TEST_CASE("reference representation query detects bundle field changes",
          "[feature:type-system][series:tsb]") {
    const auto fields = std::array{
        SchemaField{util::symbol{"first"}, &REF<TS<i64>>::type()},
        SchemaField{util::symbol{"second"}, &REF<TS<i64>>::type()},
    };
    const auto &reference_bundle = TimeSeriesBundleType::from_fields(fields, TimeSeriesGuarantee::ordinary);
    const auto value_fields = std::array{
        SchemaField{util::symbol{"first"}, &TS<i64>::type()},
        SchemaField{util::symbol{"second"}, &TS<i64>::type()},
    };
    const auto &value_bundle = TimeSeriesBundleType::from_fields(value_fields, TimeSeriesGuarantee::ordinary);

    CHECK(reference_bundle.requires_reference_representation_for(value_bundle));
}

TEST_CASE("reference representation query detects dictionary value changes",
          "[feature:type-system][series:tsd]") {
    const auto &reference_dictionary = TSD<i64, REF<TS<i64>>>::type();
    const auto &value_dictionary = TSD<i64, TS<i64>>::type();

    CHECK(reference_dictionary.requires_reference_representation_for(value_dictionary));
}

TEST_CASE("reference representation query recurses through dictionary reference values",
          "[feature:type-system][series:tsd][series:fixed-tsl]") {
    const auto &reference_dictionary = TSD<i64, REF<TSL<REF<TS<i64>>, 2>>>::type();
    const auto &value_dictionary = TSD<i64, REF<TSL<TS<i64>, 2>>>::type();

    CHECK(reference_dictionary.requires_reference_representation_for(value_dictionary));
}

TEST_CASE("common reference targets combine nested collection guarantees",
          "[feature:type-system][series:reference][series:tsd][guarantee:ordinary][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &left = REF<TSD<i64, TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>>::type();
    const auto &right = REF<TSD<i64, TS<i64>>>::type();
    const auto *common = left.common_type(right);

    REQUIRE(common != nullptr);
    const auto &reference = type_cast<TimeSeriesReferenceType>(*common);
    const auto &dictionary = type_cast<TimeSeriesDictionaryType>(reference.target_type());
    CHECK(reference.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(dictionary.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(dictionary.value_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a generic reference accepts a reference or its target",
          "[type][reference][resolution][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:const][binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = TS<i64>::type();
    const auto &reference = REF<TS<i64>, TimeSeriesGuarantee::const_>::type();
    static const auto generic_target = util::static_storage<GenericTimeSeriesValueType>{
        int64_type(), GuaranteeParameter{TimeSeriesGuarantee::ordinary}};
    [[maybe_unused]] static const auto &generic_target_registration =
        TypeRegistry::register_static(generic_target.get());
    static const auto generic_reference = util::static_storage<GenericTimeSeriesReferenceType>{
        generic_target.get(), GuaranteeParameter{TimeSeriesGuarantee::ordinary}};
    [[maybe_unused]] static const auto &generic_reference_registration =
        TypeRegistry::register_static(generic_reference.get());

    auto type_map = TypeMap{};
    CHECK(generic_reference.get().match(reference, type_map));

    type_map = TypeMap{};
    CHECK(generic_reference.get().match(target, type_map));
}

TEST_CASE("generic time series references have process-owned type identity",
          "[type][reference][resolution][feature:type-system][declaration:generic]") {
    const auto &generic_reference = generic_time_series_reference_type(TS_T::type(), TimeSeriesGuarantee::ordinary);
    CHECK(&generic_reference == &generic_time_series_reference_type(TS_T::type(), TimeSeriesGuarantee::ordinary));

    const auto &reference = REF<TS<i64>, TimeSeriesGuarantee::ordinary>::type();
    auto type_map = TypeMap{};
    REQUIRE(generic_reference.match(reference, type_map));

    const auto &resolved = static_cast<const TimeSeriesReferenceType &>(generic_reference.resolve(type_map));
    CHECK(&resolved.target_type() == &TS<i64>::type());
    CHECK(resolved.guarantee().value() == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("reference matching maps target and reference guarantees separately",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated][guarantee:final]"
          "[binding:reference-to-reference]") {
    const auto &reference = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>::type();
    static const auto target_guarantee = util::static_storage<GuaranteeVariable>{util::symbol{"target guarantee"}};
    [[maybe_unused]] static const auto &target_guarantee_registration =
        VariableRegistry::register_static(target_guarantee.get());
    static const auto reference_guarantee =
        util::static_storage<GuaranteeVariable>{util::symbol{"reference guarantee"}};
    [[maybe_unused]] static const auto &reference_guarantee_registration =
        VariableRegistry::register_static(reference_guarantee.get());
    static const auto generic_target =
        util::static_storage<GenericTimeSeriesValueType>{int64_type(), target_guarantee.get()};
    [[maybe_unused]] static const auto &generic_target_registration =
        TypeRegistry::register_static(generic_target.get());
    static const auto generic_reference =
        util::static_storage<GenericTimeSeriesReferenceType>{generic_target.get(), reference_guarantee.get()};
    [[maybe_unused]] static const auto &generic_reference_registration =
        TypeRegistry::register_static(generic_reference.get());
    auto type_map = TypeMap{};

    REQUIRE(generic_reference.get().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesReferenceType &>(generic_reference.get().resolve(type_map));

    CHECK(resolved.guarantee().value() == TimeSeriesGuarantee::deduplicated);
    CHECK(resolved.target_type().guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("dereferenced matching enforces the effective guarantee minimum",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto minimum =
        util::static_storage<GuaranteeVariable>{util::symbol{"dereferenced minimum"}, TimeSeriesGuarantee::final};
    [[maybe_unused]] static const auto &minimum_registration = VariableRegistry::register_static(minimum.get());
    static const auto parameter = util::static_storage<GenericTimeSeriesValueType>{int64_type(), minimum.get()};
    [[maybe_unused]] static const auto &parameter_registration = TypeRegistry::register_static(parameter.get());

    const auto &final_target = TS<i64, TimeSeriesGuarantee::final>::type();
    const auto &weak_reference = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type();
    const auto &strong_reference = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    CHECK_FALSE(parameter.get().match(weak_reference, type_map));

    type_map = TypeMap{};
    REQUIRE(parameter.get().match(strong_reference, type_map));
    CHECK(&parameter.get().resolve(type_map) == &final_target);
}

TEST_CASE("dereferenced matching limits each target guarantee by the reference guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]") {
    constexpr auto guarantees = std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated,
                                           TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_};
    constexpr auto expected_guarantees = std::array{
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                   TimeSeriesGuarantee::ordinary},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                   TimeSeriesGuarantee::ordinary},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final,
                   TimeSeriesGuarantee::final},
        std::array{TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final,
                   TimeSeriesGuarantee::const_},
    };

    for (auto reference_index = std::size_t{}; reference_index < guarantees.size(); ++reference_index) {
        for (auto target_index = std::size_t{}; target_index < guarantees.size(); ++target_index) {
            const auto reference_guarantee = guarantees[reference_index];
            const auto target_guarantee = guarantees[target_index];
            const auto &target = time_series_value_type(int64_type(), target_guarantee);
            const auto &reference = time_series_reference_type(target, reference_guarantee);
            auto type_map = TypeMap{};
            CAPTURE(reference_guarantee, target_guarantee);

            REQUIRE(TS_T::type().match(reference, type_map));

            const auto expected_guarantee = expected_guarantees[reference_index][target_index];
            const auto &expected = time_series_value_type(int64_type(), expected_guarantee);
            CHECK(&TS_T::type().resolve(type_map) == &expected);
        }
    }
}

namespace {

class ExtensionMatcherTimeSeriesType final : public TimeSeriesType {
  public:
    VISITOR_SUPPORT()

    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }

    inline static const TimeSeriesType *observed_type{};

  protected:
    bool do_match(const bg::Type &supplied, TypeMap &) const override {
        observed_type = type_cast<TimeSeriesType>(&supplied);
        return observed_type != nullptr;
    }
};

} // namespace

TEST_CASE("reference limits preserve the existing extension matcher path",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto extension_type = util::static_storage<ExtensionMatcherTimeSeriesType>{};
    [[maybe_unused]] static const auto &extension_type_registration =
        TypeRegistry::register_static(extension_type.get());
    const auto &final_target = TS<i64, TimeSeriesGuarantee::final>::type();
    const auto &supplied = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type();
    auto type_map = TypeMap{};
    ExtensionMatcherTimeSeriesType::observed_type = nullptr;

    REQUIRE(extension_type.get().match(final_target, type_map));
    CHECK(ExtensionMatcherTimeSeriesType::observed_type == &final_target);

    type_map = TypeMap{};
    ExtensionMatcherTimeSeriesType::observed_type = nullptr;
    REQUIRE(extension_type.get().match(supplied, type_map));
    CHECK(ExtensionMatcherTimeSeriesType::observed_type == &TS<i64>::type());
}

namespace {

bool has_ordinary_guarantee(const TimeSeriesType &type) noexcept {
    return type.guarantee().value() == TimeSeriesGuarantee::ordinary;
}

bool has_final_guarantee(const TimeSeriesType &type) noexcept {
    return type.guarantee().value() == TimeSeriesGuarantee::final;
}

} // namespace

TEST_CASE("time-series type-variable constraints inspect the observed guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    static const auto ordinary_variable = util::static_storage<TypeVariableType<TimeSeriesType>>{
        util::symbol{"ordinary observed series"}, std::vector<const TimeSeriesType *>{},
        std::vector<TypeVariableType<TimeSeriesType>::Constraint>{&has_ordinary_guarantee}};
    [[maybe_unused]] static const auto &ordinary_variable_registration =
        TypeRegistry::register_static(ordinary_variable.get());
    static const auto final_variable = util::static_storage<TypeVariableType<TimeSeriesType>>{
        util::symbol{"final observed series"}, std::vector<const TimeSeriesType *>{},
        std::vector<TypeVariableType<TimeSeriesType>::Constraint>{&has_final_guarantee}};
    [[maybe_unused]] static const auto &final_variable_registration =
        TypeRegistry::register_static(final_variable.get());
    const auto &supplied = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type();
    auto type_map = TypeMap{};

    REQUIRE(ordinary_variable.get().match(supplied, type_map));
    CHECK(&ordinary_variable.get().resolve(type_map) == &TS<i64>::type());

    type_map = TypeMap{};
    CHECK_FALSE(final_variable.get().match(supplied, type_map));
}

TEST_CASE("dereferenced guarantee variables resolve from the observed guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final]") {
    static const auto guarantee = util::static_storage<GuaranteeVariable>{util::symbol{"observed guarantee"}};
    [[maybe_unused]] static const auto &guarantee_registration = VariableRegistry::register_static(guarantee.get());
    static const auto parameter = util::static_storage<GenericTimeSeriesValueType>{int64_type(), guarantee.get()};
    [[maybe_unused]] static const auto &parameter_registration = TypeRegistry::register_static(parameter.get());

    const auto &reference = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.get().match(reference, type_map));
    CHECK(parameter.get().resolve(type_map).is_time_series());
    CHECK(static_cast<const TimeSeriesType &>(parameter.get().resolve(type_map)).guarantee().value() ==
          TimeSeriesGuarantee::final);
}

TEST_CASE("an unresolved time series cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]") {
    CHECK_THROWS_AS(TS_T::type().with_guarantee_limit(TimeSeriesGuarantee::ordinary), TypeResolutionError);
}

TEST_CASE("dereferenced matching rejects unresolved references",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[binding:reference-to-value]") {
    const auto &unresolved_target = generic_time_series_reference_type(TS_T::type(), TimeSeriesGuarantee::ordinary);
    static const auto reference_guarantee =
        util::static_storage<GuaranteeVariable>{util::symbol{"unresolved reference guarantee"}};
    [[maybe_unused]] static const auto &reference_guarantee_registration =
        VariableRegistry::register_static(reference_guarantee.get());
    static const auto unresolved_guarantee = util::static_storage<GenericTimeSeriesReferenceType>{
        TS<i64>::type(), GuaranteeParameter{reference_guarantee.get()}};
    [[maybe_unused]] static const auto &unresolved_guarantee_registration =
        TypeRegistry::register_static(unresolved_guarantee.get());
    auto type_map = TypeMap{};

    CHECK_FALSE(TS_T::type().match(unresolved_target, type_map));
    CHECK_FALSE(TS_T::type().match(unresolved_guarantee.get(), type_map));

    CHECK_THROWS_AS(unresolved_target.dereferenced_type(), TypeResolutionError);
    CHECK_THROWS_AS(unresolved_guarantee.get().dereferenced_type(), TypeResolutionError);
}

TEST_CASE("an unresolved TSL cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:fixed-tsl]") {
    CHECK_THROWS_AS((TSL<TS_T, 2>::type().with_guarantee_limit(TimeSeriesGuarantee::ordinary)), TypeResolutionError);
}

TEST_CASE("an unresolved TSD cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:tsd]") {
    CHECK_THROWS_AS((TSD<SCALAR, TS_T>::type().with_guarantee_limit(TimeSeriesGuarantee::ordinary)),
                    TypeResolutionError);
}

TEST_CASE("an unresolved TSB cannot limit its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic][series:tsb]") {
    CHECK_THROWS_AS(TSB<SCHEMA>::type().with_guarantee_limit(TimeSeriesGuarantee::ordinary), TypeResolutionError);
}

TEST_CASE("dereferenced type variables resolve to the observed time series type",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    const auto &reference = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>::type();
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    CHECK(&TS_T::type().resolve(type_map) == &TS<i64>::type());
}

TEST_CASE("nested reference item matching combines its own guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][binding:reference-to-value]") {
    using VALUE = TS<double, guarantee_variable<"Item guarantee">>;
    using DICTIONARY = TSD<i64, VALUE, guarantee_variable<"Dictionary guarantee">>;
    using TARGET_ITEM = REF<TS<double, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>;
    using ARGUMENT = TSD<i64, TARGET_ITEM>;
    const auto &parameter = DICTIONARY::type();
    const auto &argument = ARGUMENT::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(argument, type_map));
    CHECK(&parameter.resolve(type_map) == &TSD<i64, TS<double>>::type());
}

TEST_CASE("a reference limit preserves nested reference targets and caps their guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][binding:reference-to-reference]") {
    using ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, guarantee_variable<"G">>;
    using PARAMETER = TSD<i64, ITEM>;
    using SUPPLIED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = TSD<i64, SUPPLIED_ITEM>;
    using SUPPLIED = REF<SUPPLIED_TARGET, TimeSeriesGuarantee::ordinary>;
    using EXPECTED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>;
    using EXPECTED = TSD<i64, EXPECTED_ITEM>;
    const auto &parameter = PARAMETER::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(SUPPLIED::type(), type_map));
    CHECK(&parameter.resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("an outer reference limit rejects stronger nested reference guarantees",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final][guarantee:const]") {
    using ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>;
    using PARAMETER = TSD<i64, ITEM>;
    using SUPPLIED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = TSD<i64, SUPPLIED_ITEM>;
    using SUPPLIED = REF<SUPPLIED_TARGET, TimeSeriesGuarantee::ordinary>;
    auto type_map = TypeMap{};
    using CONST_PARAMETER = TSD<i64, REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>>;

    CHECK_FALSE(PARAMETER::type().match(SUPPLIED::type(), type_map));
    CHECK_FALSE(CONST_PARAMETER::type().match(SUPPLIED::type(), type_map));
}

TEST_CASE("an outer reference limit reaches TSL reference items",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:fixed-tsl][guarantee:ordinary][guarantee:final]") {
    using ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, guarantee_variable<"G">>;
    using PARAMETER = TSL<ITEM, 2>;
    using SUPPLIED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = TSL<SUPPLIED_ITEM, 2>;
    using SUPPLIED = REF<SUPPLIED_TARGET, TimeSeriesGuarantee::ordinary>;
    using EXPECTED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>;
    using EXPECTED = TSL<EXPECTED_ITEM, 2>;
    const auto &parameter = PARAMETER::type();
    auto type_map = TypeMap{};

    REQUIRE(parameter.match(SUPPLIED::type(), type_map));
    CHECK(&parameter.resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("an outer reference limit reaches TSB reference fields",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][guarantee:ordinary][guarantee:final]") {
    const auto &field_target = TS<i64, TimeSeriesGuarantee::final>::type();
    static const auto field_guarantee = util::static_storage<GuaranteeVariable>{util::symbol{"bundle field guarantee"}};
    [[maybe_unused]] static const auto &field_guarantee_registration =
        VariableRegistry::register_static(field_guarantee.get());
    static const auto generic_field =
        util::static_storage<GenericTimeSeriesReferenceType>{field_target, GuaranteeParameter{field_guarantee.get()}};
    [[maybe_unused]] static const auto &generic_field_registration = TypeRegistry::register_static(generic_field.get());
    const auto parameter_fields = std::array{SchemaField{util::symbol{"value"}, &generic_field.get()}};
    const auto &parameter_schema = anonymous_schema(parameter_fields);
    static const auto parameter =
        util::static_storage<GenericTimeSeriesBundleType>{parameter_schema, TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &parameter_registration = TypeRegistry::register_static(parameter.get());

    const auto &supplied_field = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>::type();
    const auto supplied_fields = std::array{SchemaField{util::symbol{"value"}, &supplied_field}};
    const auto &supplied_target = TimeSeriesBundleType::from_fields(supplied_fields);
    const auto &supplied = time_series_reference_type(supplied_target, TimeSeriesGuarantee::ordinary);
    auto type_map = TypeMap{};

    REQUIRE(parameter.get().match(supplied, type_map));
    const auto &resolved = static_cast<const TimeSeriesBundleType &>(parameter.get().resolve(type_map));
    const auto &resolved_field = type_cast<TimeSeriesReferenceType>(resolved.item_type(0));
    CHECK(&resolved_field.target_type() == &field_target);
    CHECK(resolved_field.guarantee().value() == TimeSeriesGuarantee::ordinary);
}

namespace {

inline constexpr auto G1 = guarantee_variable<"G1">;
inline constexpr auto G2 = guarantee_variable<"G2">;

} // namespace

TEST_CASE("top-level reference union alternatives preserve their guarantees and limited target mapping",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[guarantee:ordinary][guarantee:deduplicated][guarantee:final][guarantee:const]"
          "[binding:reference-to-reference]") {
    using GENERIC_REFERENCE = REF<TS<i64, G2>, G1>;
    using OMITTED_REFERENCE = REF<TS<i64>>;
    using ORDINARY_REFERENCE = REF<TS<i64, TimeSeriesGuarantee::ordinary>, TimeSeriesGuarantee::ordinary>;
    using DEDUPLICATED_REFERENCE = REF<TS<i64, TimeSeriesGuarantee::deduplicated>, TimeSeriesGuarantee::deduplicated>;
    using FINAL_REFERENCE = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>;
    using CONST_REFERENCE = REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>;
    using REFERENCE_UNION = TypeVariable<"REFERENCE_UNION", GENERIC_REFERENCE, OMITTED_REFERENCE, ORDINARY_REFERENCE,
                                         DEDUPLICATED_REFERENCE, FINAL_REFERENCE, CONST_REFERENCE>;

    const auto &union_type = REFERENCE_UNION::type();
    const auto verify_match = [&](const TimeSeriesType &supplied, TimeSeriesGuarantee expected_root,
                                  TimeSeriesGuarantee expected_target, const TimeSeriesType &expected_mapping) {
        auto type_map = TypeMap{};

        REQUIRE(union_type.match(supplied, type_map));
        const auto *root = std::get_if<TimeSeriesGuarantee>(type_map.find(details::guarantee_variable_instance<G1>()));
        const auto *target =
            std::get_if<TimeSeriesGuarantee>(type_map.find(details::guarantee_variable_instance<G2>()));
        REQUIRE(root != nullptr);
        REQUIRE(target != nullptr);
        CHECK(*root == expected_root);
        CHECK(*target == expected_target);
        CHECK(&union_type.resolve(type_map) == &expected_mapping);
    };

    verify_match(OMITTED_REFERENCE::type(), TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                 TS<i64>::type());
    verify_match(ORDINARY_REFERENCE::type(), TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::ordinary,
                 TS<i64>::type());
    verify_match(DEDUPLICATED_REFERENCE::type(), TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::deduplicated,
                 TS<i64>::type());
    verify_match(FINAL_REFERENCE::type(), TimeSeriesGuarantee::final, TimeSeriesGuarantee::final,
                 TS<i64, TimeSeriesGuarantee::final>::type());
    verify_match(CONST_REFERENCE::type(), TimeSeriesGuarantee::const_, TimeSeriesGuarantee::const_,
                 TS<i64, TimeSeriesGuarantee::const_>::type());

    auto rejected_type_map = TypeMap{};
    const auto &rejected = REF<TS<float, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::deduplicated>::type();
    CHECK_FALSE(union_type.match(rejected, rejected_type_map));
    CHECK(rejected_type_map.find(details::guarantee_variable_instance<G1>()) == nullptr);
    CHECK(rejected_type_map.find(details::guarantee_variable_instance<G2>()) == nullptr);
}

TEST_CASE("singleton reference unions enforce their explicit guarantees",
          "[type][reference][resolution][feature:type-system][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const][binding:reference-to-reference]") {
    using DEDUPLICATED = TypeVariable<"DEDUPLICATED", REF<TS<i64>, TimeSeriesGuarantee::deduplicated>>;
    using FINAL = TypeVariable<"FINAL", REF<TS<i64>, TimeSeriesGuarantee::final>>;
    using CONST = TypeVariable<"CONST", REF<TS<i64>, TimeSeriesGuarantee::const_>>;

    auto type_map = TypeMap{};
    CHECK(DEDUPLICATED::type().match(REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type(), type_map));
    type_map = TypeMap{};
    CHECK_FALSE(DEDUPLICATED::type().match(REF<TS<i64>, TimeSeriesGuarantee::ordinary>::type(), type_map));

    type_map = TypeMap{};
    CHECK(FINAL::type().match(REF<TS<i64>, TimeSeriesGuarantee::final>::type(), type_map));
    type_map = TypeMap{};
    CHECK_FALSE(FINAL::type().match(REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type(), type_map));

    type_map = TypeMap{};
    CHECK(CONST::type().match(REF<TS<i64>, TimeSeriesGuarantee::const_>::type(), type_map));
    type_map = TypeMap{};
    CHECK_FALSE(CONST::type().match(REF<TS<i64>, TimeSeriesGuarantee::final>::type(), type_map));
}

TEST_CASE("union alternatives match original nested reference shapes with their limit",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][binding:reference-to-reference]") {
    using ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::ordinary>;
    using DICTIONARY = TSD<i64, ITEM>;
    using ALTERNATIVE = TypeVariable<"DICTIONARY", DICTIONARY>;
    using SUPPLIED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = TSD<i64, SUPPLIED_ITEM>;
    using SUPPLIED = REF<SUPPLIED_TARGET, TimeSeriesGuarantee::ordinary>;
    const auto &variable = ALTERNATIVE::type();
    auto type_map = TypeMap{};

    REQUIRE(variable.match(SUPPLIED::type(), type_map));
    CHECK(&variable.resolve(type_map) == &DICTIONARY::type());
}

TEST_CASE("failed nested reference matching rolls back type-variable mappings",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd]") {
    using KEY = ScalarVariable<"KEY", Hashable>;
    using ITEM = REF<TS<float, TimeSeriesGuarantee::final>, guarantee_variable<"G">>;
    using PARAMETER = TSD<KEY, ITEM>;
    using SUPPLIED_ITEM = REF<TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::const_>;
    using SUPPLIED_TARGET = TSD<i64, SUPPLIED_ITEM>;
    using SUPPLIED = REF<SUPPLIED_TARGET, TimeSeriesGuarantee::ordinary>;
    auto type_map = TypeMap{};

    CHECK_FALSE(PARAMETER::type().match(SUPPLIED::type(), type_map));
    CHECK(type_map.find(KEY::type()) == nullptr);
}

TEST_CASE("dereferenced collection matching limits guarantees recursively",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][series:fixed-tsl][guarantee:ordinary][guarantee:final][guarantee:const]") {
    using TARGET = TSD<i64, TSL<TS<i64, TimeSeriesGuarantee::const_>, 2, TimeSeriesGuarantee::final>>;
    using EXPECTED = TSD<i64, TSL<TS<i64, TimeSeriesGuarantee::final>, 2, TimeSeriesGuarantee::final>>;
    const auto &reference = REF<TARGET, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    CHECK(&TS_T::type().resolve(type_map) == &EXPECTED::type());
}

TEST_CASE("a mutable reference limits stronger contained guarantees even when its root is ordinary",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:ordinary][guarantee:final]") {
    using TARGET = TSD<i64, TS<i64, TimeSeriesGuarantee::final>>;
    using EXPECTED = TSD<i64, TS<i64>>;
    const auto &reference = REF<TARGET, TimeSeriesGuarantee::ordinary>::type();
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesDictionaryType &>(TS_T::type().resolve(type_map));

    CHECK(&resolved == &EXPECTED::type());
    CHECK(resolved.key_set_type().guarantee().value() == TimeSeriesGuarantee::ordinary);
}

TEST_CASE("a const limit leaves every collection guarantee unchanged",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:const]") {
    using TARGET = TSD<i64, TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>;
    const auto &target = TARGET::type();

    CHECK(&target.with_guarantee_limit(TimeSeriesGuarantee::const_) == &target);
}

TEST_CASE("a reference guarantee also limits the dictionary key-set guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:final][guarantee:const]") {
    using TARGET = TSD<i64, TS<i64>, TimeSeriesGuarantee::const_>;
    using EXPECTED = TSD<i64, TS<i64, TimeSeriesGuarantee::final>, TimeSeriesGuarantee::final>;
    const auto &reference = REF<TARGET, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesDictionaryType &>(TS_T::type().resolve(type_map));

    CHECK(&resolved == &EXPECTED::type());
    CHECK(resolved.key_set_type().guarantee().value() == TimeSeriesGuarantee::final);
}

namespace {

template <template <typename, auto...> typename Field> struct ReferenceGuaranteeBase {
    Field<TS<i64, TimeSeriesGuarantee::const_>> base_value;
};

template <template <typename, auto...> typename Field>
struct ReferenceGuaranteeDerived : schema_extends<ReferenceGuaranteeBase, Field> {
    Field<TS<i64, TimeSeriesGuarantee::const_>> derived_value;
};

} // namespace

TEST_CASE("limiting bundle guarantees preserves named schema identity",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][schema:named][guarantee:final][guarantee:const]") {
    const auto &target = TSB<ReferenceGuaranteeDerived>::type();
    const auto &reference = time_series_reference_type(target, TimeSeriesGuarantee::final);
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesBundleType &>(TS_T::type().resolve(type_map));

    CHECK(resolved.schema().identity() == target.schema().identity());
    REQUIRE(resolved.schema().base() != nullptr);
    REQUIRE(target.schema().base() != nullptr);
    CHECK(resolved.schema().base()->identity() == target.schema().base()->identity());
    CHECK(resolved.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(resolved.item_type(1).guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("a value variable removes references after applying the outer reference limit",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsd][guarantee:final][guarantee:const][binding:reference-to-reference]") {
    using ITEM = REF<TS<i64, TimeSeriesGuarantee::const_>, TimeSeriesGuarantee::const_>;
    using TARGET = TSD<i64, ITEM>;
    const auto &reference = REF<TARGET, TimeSeriesGuarantee::final>::type();
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesDictionaryType &>(TS_T::type().resolve(type_map));

    CHECK(resolved.guarantee().value() == TimeSeriesGuarantee::ordinary);
    CHECK(&resolved.value_type() == &TS<i64, TimeSeriesGuarantee::final>::type());

    auto reference_map = TypeMap{};
    REQUIRE(REF<TS_T>::type().match(reference, reference_map));
    CHECK(&TS_T::type().resolve(reference_map) == &TARGET::type());
}

TEST_CASE("limiting a set reference caps its guarantee",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tss][guarantee:final][guarantee:const]") {
    const auto &target = time_series_set_type(int64_type(), TimeSeriesGuarantee::const_);
    const auto &reference = time_series_reference_type(target, TimeSeriesGuarantee::final);
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesSetType &>(TS_T::type().resolve(type_map));

    CHECK(resolved.guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("limiting an unnamed bundle recursively limits its fields",
          "[type][reference][resolution][feature:type-system][declaration:generic]"
          "[series:tsb][guarantee:final][guarantee:const]") {
    const auto fields = std::array{
        SchemaField{util::symbol{"first"}, &TS<i64, TimeSeriesGuarantee::const_>::type()},
        SchemaField{util::symbol{"second"}, &TS<i64, TimeSeriesGuarantee::const_>::type()},
    };
    const auto &target = TimeSeriesBundleType::from_fields(fields, TimeSeriesGuarantee::ordinary);
    const auto &reference = time_series_reference_type(target, TimeSeriesGuarantee::final);
    auto type_map = TypeMap{};

    REQUIRE(TS_T::type().match(reference, type_map));
    const auto &resolved = static_cast<const TimeSeriesBundleType &>(TS_T::type().resolve(type_map));

    CHECK_FALSE(resolved.schema().named());
    CHECK(resolved.item_type(0).guarantee().value() == TimeSeriesGuarantee::final);
    CHECK(resolved.item_type(1).guarantee().value() == TimeSeriesGuarantee::final);
}

TEST_CASE("reference matching enforces the reference guarantee",
          "[type][reference][resolution][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[binding:value-to-reference][binding:reference-to-reference]") {
    const auto &target = TS<i64>::type();
    const auto &required = REF<TS<i64>, TimeSeriesGuarantee::final>::type();
    const auto &deduplicated = REF<TS<i64>>::type();
    auto type_map = TypeMap{};

    CHECK_FALSE(required.match(deduplicated, type_map));

    type_map = TypeMap{};
    CHECK(required.match(target, type_map));
}

TEST_CASE("a reference cannot target another reference", "[type][reference][feature:type-system]") {
    CHECK_THROWS_AS((REF<REF<TS<i64>>>::type()), std::invalid_argument);
}

TEST_CASE("a value variable removes contained references but a reference target preserves them",
          "[type][reference][feature:type-system][series:tsd][schema:generic]") {
    using SOURCE = TSD<i64, REF<TS<i64>>>;
    using VALUES = TSD<i64, TS<i64>>;

    auto value_map = TypeMap{};
    REQUIRE(TS_T::type().match(SOURCE::type(), value_map));
    CHECK(&TS_T::type().resolve(value_map) == &VALUES::type());

    auto reference_map = TypeMap{};
    REQUIRE(REF<TS_T>::type().match(REF<SOURCE>::type(), reference_map));
    CHECK(&TS_T::type().resolve(reference_map) == &SOURCE::type());
}

TEST_CASE("a reference target preserves only its immediate variable's reference representation",
          "[type][reference][feature:type-system][series:tsd][schema:generic]") {
    using SOURCE = TSD<i64, REF<TSL<REF<TS<i64>>, 2>>>;
    using VALUES = TSL<TS<i64>, 2>;

    auto type_map = TypeMap{};
    REQUIRE(REF<TSD<i64, TS_T>>::type().match(REF<SOURCE>::type(), type_map));
    CHECK(&TS_T::type().resolve(type_map) == &VALUES::type());
}

namespace {

class ReferenceContainingExtensionType final : public TimeSeriesType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const TypeMap &) const override { return *this; }
    void format_name(std::string &result) const override { result += "ReferenceContainingExtension"; }
    bool contains_reference() const override { return true; }
};

} // namespace

TEST_CASE("reference-free types remove references recursively through collections",
          "[type][reference][feature:type-system][series:tsd][series:tsl]") {
    using SOURCE = TSD<i64, TSL<REF<TS<i64>>, 2>>;
    using EXPECTED = TSD<i64, TSL<TS<i64>, 2>>;

    CHECK(&SOURCE::type().reference_free_type() == &EXPECTED::type());
    CHECK(&TS<i64>::type().reference_free_type() == &TS<i64>::type());
    CHECK_THROWS_AS(TS<SCALAR>::type().reference_free_type(), TypeResolutionError);

    const auto extension_type = ReferenceContainingExtensionType{};
    CHECK_THROWS_AS(extension_type.reference_free_type(), std::invalid_argument);
}

namespace {

template <template <typename, auto...> typename Field> struct ReferenceFreeBase {
    Field<REF<TS<i64>>> base_value;
};

template <template <typename, auto...> typename Field>
struct ReferenceFreeDerived : schema_extends<ReferenceFreeBase, Field> {
    Field<TSL<REF<TS<float>>, 2>> derived_value;
};

} // namespace

TEST_CASE("reference-free TSB types preserve their named schema and base",
          "[type][reference][feature:type-system][series:tsb][schema:named]") {
    const auto &source = TSB<ReferenceFreeDerived>::type();
    const auto &result = static_cast<const TimeSeriesBundleType &>(source.reference_free_type());

    CHECK(result.schema().name() == source.schema().name());
    CHECK(result.schema().identity() == source.schema().identity());
    REQUIRE(result.schema().base() != nullptr);
    REQUIRE(source.schema().base() != nullptr);
    CHECK(result.schema().base()->identity() == source.schema().base()->identity());
    CHECK(&result.item_type(0) == &TS<i64>::type());
    const auto &items = static_cast<const TimeSeriesListType &>(result.item_type(1));
    CHECK(&items.item_type() == &TS<float>::type());
}

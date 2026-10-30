#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/node.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/guarantee_variable_instance.hpp>
#include <bg/type/scalar_variable.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/variable_registry.hpp>
#include <bg/util/static_storage.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <stdexcept>
#include <vector>

using namespace bg;

namespace {

static_assert(std::derived_from<GenericTimeSeriesValueType, TimeSeriesValueType>);
static_assert(
    !std::constructible_from<TypeVariableType<ScalarType>, util::symbol, std::vector<const TimeSeriesType *>>);
static_assert(type_cast_target<bg::Type>);
static_assert(type_cast_target<ScalarType>);
static_assert(type_cast_target<TimeSeriesListType>);
static_assert(!type_cast_target<TypeScalarType>);
static_assert(!type_cast_target<SchemaType>);
static_assert(!type_cast_target<GenericTimeSeriesVariableType>);

} // namespace

TEST_CASE("a non-reference time series is its own target type", "[type_system][feature:type-system]") {
    const auto &value = TS<float>::type();

    CHECK(&value.target_type() == &value);
}

TEST_CASE("a time series reference exposes its target type",
          "[type_system][reference][feature:type-system]") {
    const auto &target = TS<float>::type();

    CHECK(&REF<TS<float>>::type().target_type() == &target);
}

TEST_CASE("a semantic type cast preserves const", "[type_system][feature:type-system]") {
    const bg::Type *type = &TS<float>::type();

    static_assert(std::same_as<decltype(type_cast<TimeSeriesValueType>(type)), const TimeSeriesValueType *>);
    CHECK(type_cast<TimeSeriesValueType>(type) == type);
}

TEST_CASE("semantic types cast through their visitor categories", "[type_system][feature:type-system]") {
    static auto value =
        util::static_storage<GenericTimeSeriesValueType>{bg::float_type(), TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &value_registration = TypeRegistry::register_static(value.get());
    bg::Type &type = value.get();

    CHECK(type_cast<bg::Type>(&type) == &type);
    CHECK(type_cast<TimeSeriesType>(&type) == &value.get());
    CHECK(type_cast<TimeSeriesValueType>(&type) == &value.get());
    CHECK(type_cast<TimeSeriesListType>(&type) == nullptr);
    CHECK(&type_cast<TimeSeriesValueType>(type) == &value.get());
    CHECK_THROWS_AS(type_cast<TimeSeriesListType>(type), std::bad_cast);
}

TEST_CASE("evaluation time has a scalar graph type", "[type_system][feature:type-system]") {
    CHECK(&cpp_scalar_type<EvaluationTime>() == &evaluation_time_type());
    CHECK(&cpp_scalar_type<List<EvaluationTime>>() == &list_scalar_type(evaluation_time_type()));
}

TEST_CASE("time series value declarations require canonical integer types",
          "[feature:type-system][series:value][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(details::supported_time_series<TS<int>>);
    STATIC_CHECK_FALSE(details::supported_time_series<TS<List<int>>>);
    STATIC_CHECK(details::supported_time_series<TS<i64>>);
}

TEST_CASE("scalar graph types report equality and hashing", "[type_system][feature:type-system]") {
    CHECK(int64_type().supports_equality());
    CHECK(int64_type().supports_hash());
    CHECK(list_scalar_type(int64_type()).supports_equality());
    CHECK(list_scalar_type(int64_type()).supports_hash());

    const auto &opaque = cpp_scalar_type<Opaque<int>>();
    CHECK_FALSE(opaque.supports_equality());
    CHECK_FALSE(opaque.supports_hash());
}

TEST_CASE("built-in scalar types do not report value covariance", "[feature:type-system][scalar:i64]") {
    CHECK_FALSE(bg::int64_type().supports_value_covariance());
}

TEST_CASE("collection scalar types do not report value covariance", "[feature:type-system][scalar:list]") {
    CHECK_FALSE(bg::cpp_scalar_type<bg::List<bg::i64>>().supports_value_covariance());
}

TEST_CASE("a generic time-series type retains its guarantee variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const ScalarType &float_type = bg::float_type();

    using FLOAT_WITH_VARIABLE_GUARANTEE = TS<float, guarantee_variable<"G">>;
    const auto &ts_float = FLOAT_WITH_VARIABLE_GUARANTEE::type();

    REQUIRE_FALSE(ts_float.is_resolved());
    CHECK(&ts_float.value_type() == &float_type);
    CHECK(ts_float.guarantee().variable() == &details::guarantee_variable_instance<guarantee_variable<"G">>());
}

TEST_CASE("guarantee variable declarations retain their minimum guarantee",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated][guarantee:final]"
          "[guarantee:const]") {
    using UNCONSTRAINED = decltype(guarantee_variable<"MINIMUM_G">);
    using ORDINARY = decltype(guarantee_variable<"MINIMUM_G", TimeSeriesGuarantee::ordinary>);
    using DEDUPLICATED = decltype(guarantee_variable<"MINIMUM_G", TimeSeriesGuarantee::deduplicated>);

    const auto &unconstrained = details::guarantee_variable_instance<UNCONSTRAINED>();
    const auto &ordinary = details::guarantee_variable_instance<ORDINARY>();
    const auto &deduplicated = details::guarantee_variable_instance<DEDUPLICATED>();

    CHECK(unconstrained.minimum() == std::nullopt);
    CHECK(ordinary.minimum() == TimeSeriesGuarantee::ordinary);
    CHECK(deduplicated.minimum() == TimeSeriesGuarantee::deduplicated);
    CHECK(&unconstrained != &ordinary);
    CHECK(&unconstrained != &deduplicated);
    CHECK(&ordinary != &deduplicated);

    for (auto supplied : {TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final,
                          TimeSeriesGuarantee::const_}) {
        auto type_map = TypeMap{};
        REQUIRE(unconstrained.match(supplied, type_map));
        CHECK(unconstrained.resolve(type_map) == supplied);
    }

    const auto check_minimum = []<TimeSeriesGuarantee Minimum>(auto declaration) {
        const auto &variable = details::guarantee_variable_instance<decltype(declaration)>();
        CHECK(variable.minimum() == Minimum);

        for (auto supplied : {TimeSeriesGuarantee::ordinary, TimeSeriesGuarantee::deduplicated,
                              TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_}) {
            auto type_map = TypeMap{};
            const auto accepted = satisfies(supplied, Minimum);
            CHECK(variable.match(supplied, type_map) == accepted);
            if (accepted) {
                CHECK(variable.resolve(type_map) == supplied);
            }
        }
    };

    check_minimum.template operator()<TimeSeriesGuarantee::ordinary>(
        guarantee_variable<"ORDINARY_MINIMUM_G", TimeSeriesGuarantee::ordinary>);
    check_minimum.template operator()<TimeSeriesGuarantee::deduplicated>(
        guarantee_variable<"DEDUPLICATED_MINIMUM_G", TimeSeriesGuarantee::deduplicated>);
    check_minimum.template operator()<TimeSeriesGuarantee::final>(
        guarantee_variable<"FINAL_MINIMUM_G", TimeSeriesGuarantee::final>);
    check_minimum.template operator()<TimeSeriesGuarantee::const_>(
        guarantee_variable<"CONST_MINIMUM_G", TimeSeriesGuarantee::const_>);
}

TEST_CASE("a constrained guarantee variable resolves to the weakest accepted supplied guarantee",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated]"
          "[guarantee:final][guarantee:const]") {
    using DEDUPLICATED = TS<i64, guarantee_variable<"REPEATED_MINIMUM_G", TimeSeriesGuarantee::deduplicated>>;
    const auto &generic = DEDUPLICATED::type();

    for (auto supplied : {TimeSeriesGuarantee::deduplicated, TimeSeriesGuarantee::final, TimeSeriesGuarantee::const_}) {
        auto type_map = TypeMap{};
        const auto &concrete = TS<i64>::type().with_guarantee(supplied);

        REQUIRE(generic.match(concrete, type_map));
        CHECK(&generic.resolve(type_map) == &concrete);
    }

    auto type_map = TypeMap{};
    CHECK_FALSE(generic.match(TS<i64>::type(), type_map));

    type_map = TypeMap{};
    REQUIRE(generic.match(TS<i64, TimeSeriesGuarantee::const_>::type(), type_map));
    REQUIRE(generic.match(TS<i64, TimeSeriesGuarantee::final>::type(), type_map));
    CHECK(&generic.resolve(type_map) == &TS<i64, TimeSeriesGuarantee::final>::type());
}

TEST_CASE("generic C++ declarations expose their semantic types",
          "[type_system][feature:type-system][declaration:generic]") {
    struct SignatureScope {};

    const auto &value = TS<SCALAR>::type();
    const auto &parameter = details::time_series_type_for<SignatureScope, 0, TimeSeriesTypeRole::input, TS<SCALAR>>(
        TimeSeriesGuarantee::ordinary);
    const auto &time_series = TS_T::type();
    auto integer_bindings = TypeMap{};
    auto float_bindings = TypeMap{};

    STATIC_CHECK(std::same_as<decltype(value), const TimeSeriesValueType &>);
    CHECK_FALSE(value.is_resolved());
    CHECK_FALSE(time_series.is_resolved());
    CHECK(&value.value_type() == &static_cast<const TimeSeriesValueType &>(parameter).value_type());

    REQUIRE(value.match(TS<i64>::type(), integer_bindings));
    REQUIRE(value.match(TS<float>::type(), float_bindings));
    CHECK(&value.resolve(integer_bindings) == &TS<i64>::type());
    CHECK(&value.resolve(float_bindings) == &TS<float>::type());
}

TEST_CASE("type compatibility can be checked without retaining a type map",
          "[type_system][feature:type-system][declaration:generic]") {
    CHECK(TS<SCALAR>::type().accepts(TS<i64>::type()));
    CHECK_FALSE(TS<i64>::type().accepts(TS<float>::type()));
}

TEST_CASE("compatible time-series types have a common weakest guarantee",
          "[type_system][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto &ordinary = TS<i64>::type();
    const auto &final = TS<i64, TimeSeriesGuarantee::final>::type();
    const auto &constant = TS<i64, TimeSeriesGuarantee::const_>::type();

    CHECK(final.accepts(constant));
    CHECK_FALSE(constant.accepts(final));

    CHECK(final.common_type(ordinary) == &ordinary);
    CHECK(final.common_type(constant) == &final);
    CHECK(ordinary.common_type(TS<float>::type()) == nullptr);
    CHECK(TS<SCALAR>::type().common_type(ordinary) == nullptr);
    CHECK(ordinary.common_type(TS<SCALAR>::type()) == nullptr);
}

TEST_CASE("an ordinary time-series input accepts final and const values",
          "[type_system][feature:type-system][series:value][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    struct InputScope {};

    const auto &ordinary_input =
        details::time_series_type_for<InputScope, 0, TimeSeriesTypeRole::input, TS<i64, TimeSeriesGuarantee::ordinary>>(
            TimeSeriesGuarantee::ordinary);

    CHECK(ordinary_input.accepts(TS<i64, TimeSeriesGuarantee::final>::type()));
    CHECK(ordinary_input.accepts(TS<i64, TimeSeriesGuarantee::const_>::type()));
}

TEST_CASE("a resolved ordinary time-series value accepts stronger guarantees",
          "[type_system][feature:type-system][series:value][guarantee:ordinary][guarantee:const]") {
    const auto &ordinary = TS<i64>::type();
    const auto &final = TS<i64, TimeSeriesGuarantee::final>::type();

    CHECK(ordinary.accepts(final));
    CHECK(ordinary.accepts(TS<i64, TimeSeriesGuarantee::const_>::type()));
    CHECK_FALSE(final.accepts(ordinary));
}

TEST_CASE("an omitted time-series variable guarantee preserves the supplied guarantee",
          "[type_system][feature:type-system][declaration:generic][series:value][guarantee:final]") {
    const auto identity = node([](In<TS_T>, Out<TS_T>) {});
    const auto arguments = std::array<const bg::Type *, 1>{&TS<i64, TimeSeriesGuarantee::final>::type()};

    const auto resolved = identity.signature().resolve(arguments);

    CHECK(resolved.parameters[0].type == &TS<i64, TimeSeriesGuarantee::final>::type());
    CHECK(resolved.output == &TS<i64, TimeSeriesGuarantee::final>::type());
}

TEST_CASE("a time series variable and its guarantee resolve independently",
          "[type_system][feature:type-system][declaration:generic][guarantee:ordinary][guarantee:final]") {
    using LEFT = TimeSeriesVariable<"Left">;
    using RIGHT = TimeSeriesVariable<"Right">;
    using LEFT_WITH_GUARANTEE = LEFT::with<guarantee_variable<"G">>;
    using RIGHT_WITH_GUARANTEE = RIGHT::with<guarantee_variable<"G">>;
    const auto &generic_left = LEFT_WITH_GUARANTEE::type();
    const auto &generic_right = RIGHT_WITH_GUARANTEE::type();
    const auto &list = TSL<TS<float, TimeSeriesGuarantee::ordinary>, 2, TimeSeriesGuarantee::final>::type();
    const auto &value = TS<i64>::type();
    auto type_map = TypeMap{};

    REQUIRE(generic_left.match(list, type_map));
    REQUIRE(generic_right.match(value, type_map));

    CHECK(&generic_left.resolve(type_map) == &TSL<TS<float, TimeSeriesGuarantee::final>, 2>::type());
    CHECK(&generic_right.resolve(type_map) == &value);
}

TEST_CASE("a deduplicated value time series requires equality",
          "[type_system][feature:type-system][series:value][scalar:opaque][guarantee:deduplicated]") {
    const auto &opaque = cpp_scalar_type<Opaque<int>>();
    CHECK_THROWS_AS(time_series_value_type(opaque, TimeSeriesGuarantee::deduplicated), TypeResolutionError);

    const auto &final_type = time_series_value_type(opaque, TimeSeriesGuarantee::final);
    CHECK(&final_type.value_type() == &opaque);
    CHECK(final_type.guarantee().value() == TimeSeriesGuarantee::final);

    const auto &const_type = time_series_value_type(opaque, TimeSeriesGuarantee::const_);
    CHECK(&const_type.value_type() == &opaque);
    CHECK(const_type.guarantee().value() == TimeSeriesGuarantee::const_);
}

TEST_CASE("a time series variable ignores the guarantees captured separately",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated]"
          "[guarantee:final]") {
    using VARIABLE_WITH_GUARANTEE = TS_T::with<guarantee_variable<"G">>;
    const auto &generic = VARIABLE_WITH_GUARANTEE::type();
    const auto &final = TS<float, TimeSeriesGuarantee::final>::type();
    const auto &deduplicated = TS<float, TimeSeriesGuarantee::deduplicated>::type();
    auto type_map = TypeMap{};

    REQUIRE(generic.match(final, type_map));
    REQUIRE(generic.match(deduplicated, type_map));

    CHECK(&generic.resolve(type_map) == &deduplicated);
}

TEST_CASE("resolved time-series value types are reused", "[type_system][feature:type-system][guarantee:final]") {
    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();

    CHECK(&ts_final == &TS<float, TimeSeriesGuarantee::final>::type());
}

TEST_CASE("signature resolution uses reference target types",
          "[type_system][reference][feature:type-system][binding:reference-to-value]") {
    const auto &target = TS<i64>::type();
    const auto &reference = REF<TS<i64>>::type();
    const auto value_input = Signature{util::symbol{"value_input"}, {{util::symbol{"value"}, &target}}};
    const auto reference_input = Signature{util::symbol{"reference_input"}, {{util::symbol{"value"}, &reference}}};
    const auto arguments = std::array<const bg::Type *, 1>{&reference};

    CHECK(value_input.resolve(arguments).parameters[0].type == &target);
    CHECK(reference_input.resolve(arguments).parameters[0].type == &reference);
}

TEST_CASE("signature mismatches report the signature, parameter, expected type, and supplied type",
          "[type_system][feature:type-system]") {
    const auto signature = Signature{util::symbol{"consume"}, {{util::symbol{"value"}, &TS<i64>::type()}}};
    const auto arguments = std::array<const Type *, 1>{&TS<string>::type()};

    try {
        static_cast<void>(signature.match_arguments(arguments));
        FAIL("expected a signature argument mismatch");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
        CHECK(error.expected() == &TS<i64>::type());
        CHECK(error.supplied() == &TS<string>::type());
        const auto message = std::string{error.what()};
        CHECK(message.contains("signature 'consume'"));
        CHECK(message.contains("parameter 'value'"));
        CHECK(message.contains("expected TS<i64>"));
        CHECK(message.contains("supplied TS<string>"));
    }
}

TEST_CASE("a resolved parameter constrains a generic argument",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto signature = Signature{util::symbol{"fixed"}, {{util::symbol{"value"}, &TS<float>::type()}}};
    const auto arguments = std::array<const bg::Type *, 1>{&TS_T::type()};

    const auto resolution = signature.resolve_with_type_map(arguments);

    CHECK(resolution.signature.parameters[0].type == &TS<float>::type());
    CHECK(&TS_T::type().resolve(resolution.type_map) == &TS<float>::type());
}

TEST_CASE("resolved arguments constrain a shared parameter before generic arguments",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto signature =
        Signature{util::symbol{"same"}, {{util::symbol{"lhs"}, &TS_T::type()}, {util::symbol{"rhs"}, &TS_T::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&TS_T::type(), &TS<float>::type()};

    const auto resolution = signature.resolve_with_type_map(arguments);

    CHECK(resolution.signature.parameters[0].type == &TS<float>::type());
    CHECK(resolution.signature.parameters[1].type == &TS<float>::type());
}

TEST_CASE("signature resolution rejects mutually unconstrained argument and parameter variables",
          "[type_system][feature:type-system][declaration:generic]") {
    using PARAMETER = TimeSeriesVariable<"PARAMETER">::with<TimeSeriesGuarantee::ordinary>;
    using ARGUMENT = TimeSeriesVariable<"ARGUMENT">::with<TimeSeriesGuarantee::ordinary>;
    const auto &parameter = PARAMETER::type();
    const auto &argument = ARGUMENT::type();
    const auto signature = Signature{util::symbol{"unconstrained"}, {{util::symbol{"value"}, &parameter}}};
    const auto arguments = std::array<const bg::Type *, 1>{&argument};

    CHECK_THROWS_AS(signature.resolve(arguments), TypeResolutionError);
}

namespace {

[[nodiscard]] bool is_value_type(const bg::Type *type, const ScalarType &value_type, TimeSeriesGuarantee guarantee) {
    const auto *time_series = type_cast<TimeSeriesValueType>(type);

    return time_series != nullptr && &time_series->value_type() == &value_type &&
           time_series->guarantee().value() == guarantee;
}

} // namespace

TEST_CASE("a signature resolves scalar parameters", "[type_system][feature:type-system]") {
    const ScalarType &float_type = bg::float_type();

    using TS_FLOAT = TS<float, guarantee_variable<"G">>;
    using LHS = Union<"Lhs", float, TS_FLOAT>;
    using RHS = Union<"Rhs", float, TS_FLOAT>;
    const auto &ts_float = TS_FLOAT::type();
    const auto *guarantee = ts_float.guarantee().variable();
    const auto &lhs_type = LHS::type();
    const auto &rhs_type = RHS::type();

    const Signature add{util::symbol{"add"},
                        {{util::symbol{"lhs"}, &lhs_type}, {util::symbol{"rhs"}, &rhs_type}},
                        ts_float,
                        {{guarantee, [](const TypeMap &) -> TypeMapValue { return TimeSeriesGuarantee::const_; }}}};

    const std::array<const bg::Type *, 2> scalar_arguments{&float_type, &float_type};
    const auto scalar_add = add.resolve(scalar_arguments);

    CHECK(scalar_add.parameters[0].type->is_scalar());
    CHECK(scalar_add.parameters[1].type->is_scalar());
    CHECK(is_value_type(scalar_add.output, float_type, TimeSeriesGuarantee::const_));
}

TEST_CASE("a signature resolves scalar and time-series parameters", "[type_system][feature:type-system]") {
    const ScalarType &float_type = bg::float_type();

    using TS_FLOAT = TS<float, guarantee_variable<"G">>;
    using LHS = Union<"Lhs", float, TS_FLOAT>;
    using RHS = Union<"Rhs", float, TS_FLOAT>;
    const auto &ts_float = TS_FLOAT::type();
    const auto &lhs_type = LHS::type();
    const auto &rhs_type = RHS::type();
    const Signature add{
        util::symbol{"add"}, {{util::symbol{"lhs"}, &lhs_type}, {util::symbol{"rhs"}, &rhs_type}}, ts_float};

    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const std::array<const bg::Type *, 2> mixed_arguments{&float_type, &ts_final};
    const auto mixed_add = add.resolve(mixed_arguments);

    CHECK(mixed_add.parameters[0].type->is_scalar());
    CHECK(mixed_add.parameters[1].type->is_time_series());
    CHECK(mixed_add.parameters[1].type == &ts_final);
    CHECK(is_value_type(mixed_add.output, float_type, TimeSeriesGuarantee::final));
}

TEST_CASE("a shared guarantee variable resolves to the weakest input guarantee",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated]"
          "[guarantee:final]") {
    const ScalarType &float_type = bg::float_type();

    using TS_FLOAT = TS<float, guarantee_variable<"G">>;
    using LHS = Union<"Lhs", TS_FLOAT>;
    using RHS = Union<"Rhs", TS_FLOAT>;
    const auto &ts_float = TS_FLOAT::type();
    const auto &lhs_type = LHS::type();
    const auto &rhs_type = RHS::type();
    const Signature add{
        util::symbol{"add"}, {{util::symbol{"lhs"}, &lhs_type}, {util::symbol{"rhs"}, &rhs_type}}, ts_float};

    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const auto &ts_deduplicated = TS<float, TimeSeriesGuarantee::deduplicated>::type();
    const std::array<const bg::Type *, 2> series_arguments{&ts_final, &ts_deduplicated};
    const auto series_add = add.resolve(series_arguments);

    CHECK(series_add.parameters[0].type->is_time_series());
    CHECK(series_add.parameters[1].type->is_time_series());
    CHECK(is_value_type(series_add.output, float_type, TimeSeriesGuarantee::deduplicated));
}

TEST_CASE("a guarantee variable enforces its lower bound",
          "[type_system][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final]") {
    const ScalarType &float_type = bg::float_type();

    static const auto stable_guarantee =
        util::static_storage<GuaranteeVariable>{util::symbol{"Stable"}, TimeSeriesGuarantee::deduplicated};
    [[maybe_unused]] static const auto &stable_guarantee_registration =
        VariableRegistry::register_static(stable_guarantee.get());
    static const auto stable_ts_float =
        util::static_storage<GenericTimeSeriesValueType>{float_type, stable_guarantee.get()};
    [[maybe_unused]] static const auto &stable_ts_float_registration =
        TypeRegistry::register_static(stable_ts_float.get());
    static const auto stable_type = util::static_storage<TypeVariableType<bg::Type>>{
        util::symbol{"StableType"}, std::vector<const bg::Type *>{&stable_ts_float.get()}};
    [[maybe_unused]] static const auto &stable_type_registration = TypeRegistry::register_static(stable_type.get());
    const Signature stable_input{
        util::symbol{"stable_input"}, {{util::symbol{"value"}, &stable_type.get()}}, stable_ts_float.get()};

    const auto &ts_ordinary = TS<float>::type();
    const std::array<const bg::Type *, 1> weak_argument{&ts_ordinary};

    try {
        static_cast<void>(stable_input.resolve(weak_argument));
        FAIL("ordinary guarantee satisfied a deduplicated lower bound");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
    }

    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const std::array<const bg::Type *, 1> stable_argument{&ts_final};
    const auto resolved_stable_input = stable_input.resolve(stable_argument);
    CHECK(is_value_type(resolved_stable_input.output, float_type, TimeSeriesGuarantee::final));
}

TEST_CASE("repeated use of a type variable requires one resolved type",
          "[type_system][feature:type-system][declaration:generic]") {
    const ScalarType &float_type = bg::float_type();
    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();
    const auto &ts_ordinary = TS<float>::type();

    static const auto same_type = util::static_storage<TypeVariableType<bg::Type>>{util::symbol{"Same"}};
    [[maybe_unused]] static const auto &same_type_registration = TypeRegistry::register_static(same_type.get());
    const Signature same_inputs{util::symbol{"same_inputs"},
                                {{util::symbol{"first"}, &same_type.get()}, {util::symbol{"second"}, &same_type.get()}},
                                ts_ordinary};
    const std::array<const bg::Type *, 2> different_arguments{&float_type, &ts_final};

    try {
        static_cast<void>(same_inputs.resolve(different_arguments));
        FAIL("one type variable resolved to different argument types");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "second");
    }
}

TEST_CASE("a type parameter maps a time-series type variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto &ts_final = TS<float, TimeSeriesGuarantee::final>::type();

    const auto &selected_type = TS_T::type();
    static const auto type_parameter = util::static_storage<GenericTypeScalarType>{selected_type};
    [[maybe_unused]] static const auto &type_parameter_registration =
        TypeRegistry::register_static(type_parameter.get());
    const Signature select_type{
        util::symbol{"select_type"}, {{util::symbol{"type"}, &type_parameter.get()}}, selected_type};
    const TypeScalarType &final_type_argument = type_of(ts_final);
    const std::array<const bg::Type *, 1> final_type_arguments{&final_type_argument};
    const auto selected_final = select_type.resolve(final_type_arguments);

    CHECK(selected_final.parameters[0].type->is_scalar());
    CHECK(selected_final.parameters[0].type == &final_type_argument);
    CHECK(selected_final.output == &ts_final);
}

TEST_CASE("a type parameter maps a nested guarantee variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const ScalarType &float_type = bg::float_type();
    const auto &ts_deduplicated = TS<float, TimeSeriesGuarantee::deduplicated>::type();

    using SELECTED_TS_FLOAT = TS<float, guarantee_variable<"SelectedGuarantee">>;
    const auto &selected_ts_float = SELECTED_TS_FLOAT::type();
    static const auto constrained_type_parameter = util::static_storage<GenericTypeScalarType>{selected_ts_float};
    [[maybe_unused]] static const auto &constrained_type_parameter_registration =
        TypeRegistry::register_static(constrained_type_parameter.get());
    const Signature select_guarantee{util::symbol{"select_guarantee"},
                                     {{util::symbol{"type"}, &constrained_type_parameter.get()}},
                                     selected_ts_float};
    const TypeScalarType &deduplicated_type_argument = type_of(ts_deduplicated);
    const std::array<const bg::Type *, 1> deduplicated_type_arguments{&deduplicated_type_argument};
    const auto selected_deduplicated = select_guarantee.resolve(deduplicated_type_arguments);

    CHECK(is_value_type(selected_deduplicated.output, float_type, TimeSeriesGuarantee::deduplicated));
}

TEST_CASE("a time-series type parameter rejects a scalar type", "[type_system][feature:type-system]") {
    const ScalarType &float_type = bg::float_type();
    using SELECTED_TS_FLOAT = TS<float, guarantee_variable<"SelectedGuarantee">>;
    const auto &selected_ts_float = SELECTED_TS_FLOAT::type();
    static const auto constrained_type_parameter = util::static_storage<GenericTypeScalarType>{selected_ts_float};
    [[maybe_unused]] static const auto &constrained_type_parameter_registration =
        TypeRegistry::register_static(constrained_type_parameter.get());
    const Signature select_guarantee{util::symbol{"select_guarantee"},
                                     {{util::symbol{"type"}, &constrained_type_parameter.get()}},
                                     selected_ts_float};

    const TypeScalarType &scalar_type_argument = type_of(float_type);
    const std::array<const bg::Type *, 1> scalar_type_arguments{&scalar_type_argument};

    try {
        static_cast<void>(select_guarantee.resolve(scalar_type_arguments));
        FAIL("a scalar type satisfied a time-series type parameter");
    } catch (const TypeResolutionError &error) {
        CHECK(error.code() == TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "type");
    }
}

TEST_CASE("a generic scalar list resolves its item type", "[type_system][feature:type-system][declaration:generic]") {
    using ITEM = ScalarVariable<"Item">;
    const auto &generic_list = cpp_scalar_type<List<ITEM>>();
    const auto &float_list = list_scalar_type(bg::float_type());
    auto type_map = TypeMap{};

    REQUIRE(generic_list.match(float_list, type_map));
    CHECK(&generic_list.resolve(type_map) == &float_list);
}

TEST_CASE("generic set and dictionary types resolve their components",
          "[type_system][feature:type-system][declaration:generic]") {
    using KEY = ScalarVariable<"Key">;
    using VALUE = ScalarVariable<"Value">;
    const auto &generic_set = cpp_scalar_type<Set<KEY>>();
    const auto &generic_dict = cpp_scalar_type<Dict<KEY, VALUE>>();
    const auto &resolved_set = set_scalar_type(string_type());
    const auto &resolved_dict = dict_scalar_type(string_type(), int64_type());

    auto set_bindings = TypeMap{};
    REQUIRE(generic_set.match(resolved_set, set_bindings));
    CHECK(&generic_set.resolve(set_bindings) == &resolved_set);

    auto dict_bindings = TypeMap{};
    REQUIRE(generic_dict.match(resolved_dict, dict_bindings));
    CHECK(&generic_dict.resolve(dict_bindings) == &resolved_dict);
}

TEST_CASE("set items and dictionary keys require hashing", "[type_system][scalar][collection][feature:type-system]") {
    const auto &opaque = cpp_scalar_type<Opaque<int>>();

    CHECK_THROWS_AS(set_scalar_type(opaque), std::invalid_argument);
    CHECK_THROWS_AS(dict_scalar_type(opaque, int64_type()), std::invalid_argument);
}

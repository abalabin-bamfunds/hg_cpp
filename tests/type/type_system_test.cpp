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
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <stdexcept>
#include <vector>

namespace {

static_assert(std::derived_from<bg::GenericTimeSeriesValueType, bg::TimeSeriesValueType>);
static_assert(!std::constructible_from<bg::TypeVariableType<bg::ScalarType>, bg::util::symbol,
                                       std::vector<const bg::TimeSeriesType *>>);
static_assert(bg::type_cast_target<bg::Type>);
static_assert(bg::type_cast_target<bg::ScalarType>);
static_assert(bg::type_cast_target<bg::TimeSeriesListType>);
static_assert(!bg::type_cast_target<bg::TypeScalarType>);
static_assert(!bg::type_cast_target<bg::BundleSchemaType>);
static_assert(!bg::type_cast_target<bg::GenericTimeSeriesVariableType>);

[[nodiscard]] bool is_value_type(const bg::Type *type, const bg::ScalarType &value_type,
                                 bg::TimeSeriesGuarantee guarantee) {
    const auto *time_series = bg::type_cast<bg::TimeSeriesValueType>(type);

    return time_series != nullptr && &time_series->value_type() == &value_type &&
           time_series->guarantee().value() == guarantee;
}

} // namespace

TEST_CASE("a non-reference time series is its own target type", "[type_system][feature:type-system]") {
    const auto &value = bg::TS<float>::type();

    CHECK(&value.target_type() == &value);
}

TEST_CASE("a time series reference exposes its target type",
          "[type_system][reference][feature:type-system]") {
    const auto &target = bg::TS<float>::type();

    CHECK(&bg::REF<bg::TS<float>>::type().target_type() == &target);
}

TEST_CASE("a semantic type cast preserves const", "[type_system][feature:type-system]") {
    const bg::Type *type = &bg::TS<float>::type();

    static_assert(std::same_as<decltype(bg::type_cast<bg::TimeSeriesValueType>(type)),
                               const bg::TimeSeriesValueType *>);
    CHECK(bg::type_cast<bg::TimeSeriesValueType>(type) == type);
}

TEST_CASE("semantic types cast through their visitor categories", "[type_system][feature:type-system]") {
    auto value = bg::GenericTimeSeriesValueType{bg::float_type(), bg::TimeSeriesGuarantee::ordinary};
    bg::Type &type = value;

    CHECK(bg::type_cast<bg::Type>(&type) == &type);
    CHECK(bg::type_cast<bg::TimeSeriesType>(&type) == &value);
    CHECK(bg::type_cast<bg::TimeSeriesValueType>(&type) == &value);
    CHECK(bg::type_cast<bg::TimeSeriesListType>(&type) == nullptr);
    CHECK(&bg::type_cast<bg::TimeSeriesValueType>(type) == &value);
    CHECK_THROWS_AS(bg::type_cast<bg::TimeSeriesListType>(type), std::bad_cast);
}

TEST_CASE("evaluation time has a scalar graph type", "[type_system][feature:type-system]") {
    CHECK(&bg::cpp_scalar_type<bg::EvaluationTime>() == &bg::evaluation_time_type());
    CHECK(&bg::cpp_scalar_type<bg::List<bg::EvaluationTime>>() == &bg::list_scalar_type(bg::evaluation_time_type()));
}

TEST_CASE("time series value declarations require canonical integer types",
          "[feature:type-system][series:value][scalar:integer][declaration:concrete]") {
    STATIC_CHECK_FALSE(bg::details::supported_time_series<bg::TS<int>>);
    STATIC_CHECK_FALSE(bg::details::supported_time_series<bg::TS<bg::List<int>>>);
    STATIC_CHECK(bg::details::supported_time_series<bg::TS<bg::i64>>);
}

TEST_CASE("scalar graph types report equality and hashing", "[type_system][feature:type-system]") {
    CHECK(bg::int64_type().supports_equality());
    CHECK(bg::int64_type().supports_hash());
    CHECK(bg::list_scalar_type(bg::int64_type()).supports_equality());
    CHECK(bg::list_scalar_type(bg::int64_type()).supports_hash());

    const auto &opaque = bg::cpp_scalar_type<bg::Opaque<int>>();
    CHECK_FALSE(opaque.supports_equality());
    CHECK_FALSE(opaque.supports_hash());
}

TEST_CASE("a generic time-series type retains its guarantee variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};

    REQUIRE_FALSE(ts_float.is_resolved());
    CHECK(&ts_float.value_type() == &float_type);
    CHECK(ts_float.guarantee().variable() == &guarantee);
}

TEST_CASE("generic C++ declarations expose their semantic types",
          "[type_system][feature:type-system][declaration:generic]") {
    struct SignatureScope {};

    const auto &value = bg::TS<bg::SCALAR>::type();
    const auto &parameter = bg::details::time_series_type_for<SignatureScope, 0, true, bg::TS<bg::SCALAR>>(
        bg::TimeSeriesGuarantee::ordinary);
    const auto &time_series = bg::TS_T::type();
    auto integer_bindings = bg::TypeMap{};
    auto float_bindings = bg::TypeMap{};

    STATIC_CHECK(std::same_as<decltype(value), const bg::TimeSeriesValueType &>);
    CHECK_FALSE(value.is_resolved());
    CHECK_FALSE(time_series.is_resolved());
    CHECK(&value.value_type() == &static_cast<const bg::TimeSeriesValueType &>(parameter).value_type());

    REQUIRE(value.match(bg::TS<bg::i64>::type(), integer_bindings));
    REQUIRE(value.match(bg::TS<float>::type(), float_bindings));
    CHECK(&value.resolve(integer_bindings) == &bg::TS<bg::i64>::type());
    CHECK(&value.resolve(float_bindings) == &bg::TS<float>::type());
}

TEST_CASE("type compatibility can be checked without retaining a type map",
          "[type_system][feature:type-system][declaration:generic]") {
    CHECK(bg::TS<bg::SCALAR>::type().accepts(bg::TS<bg::i64>::type()));
    CHECK_FALSE(bg::TS<bg::i64>::type().accepts(bg::TS<float>::type()));
}

TEST_CASE("compatible time-series types have a common weakest guarantee",
          "[type_system][feature:type-system][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    const auto &ordinary = bg::TS<bg::i64>::type();
    const auto &final = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();
    const auto &constant = bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type();

    CHECK(final.accepts(constant));
    CHECK_FALSE(constant.accepts(final));

    CHECK(final.common_type(ordinary) == &ordinary);
    CHECK(final.common_type(constant) == &final);
    CHECK(ordinary.common_type(bg::TS<float>::type()) == nullptr);
}

TEST_CASE("an ordinary time-series input accepts final and const values",
          "[type_system][feature:type-system][series:value][guarantee:ordinary][guarantee:final]"
          "[guarantee:const]") {
    struct InputScope {};

    const auto &ordinary_input = bg::details::time_series_type_for<InputScope, 0, true,
                                                                   bg::TS<bg::i64, bg::TimeSeriesGuarantee::ordinary>>(
        bg::TimeSeriesGuarantee::ordinary);

    CHECK(ordinary_input.accepts(bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()));
    CHECK(ordinary_input.accepts(bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()));
}

TEST_CASE("a resolved ordinary time-series value accepts stronger guarantees",
          "[type_system][feature:type-system][series:value][guarantee:ordinary][guarantee:const]") {
    const auto &ordinary = bg::TS<bg::i64>::type();
    const auto &final = bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type();

    CHECK(ordinary.accepts(final));
    CHECK(ordinary.accepts(bg::TS<bg::i64, bg::TimeSeriesGuarantee::const_>::type()));
    CHECK_FALSE(final.accepts(ordinary));
}

TEST_CASE("an omitted time-series variable guarantee preserves the supplied guarantee",
          "[type_system][feature:type-system][declaration:generic][series:value][guarantee:final]") {
    const auto identity = bg::node([](bg::In<bg::TS_T>, bg::Out<bg::TS_T>) {});
    const auto arguments = std::array<const bg::Type *, 1>{
        &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type()};

    const auto resolved = identity.signature().resolve(arguments);

    CHECK(resolved.parameters[0].type == &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    CHECK(resolved.output == &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("a time series variable and its guarantee resolve independently",
          "[type_system][feature:type-system][declaration:generic][guarantee:ordinary][guarantee:final]") {
    auto left = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"Left"}};
    auto right = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"Right"}};
    auto guarantee = bg::GuaranteeVariable{bg::util::symbol{"G"}};
    const auto generic_left = bg::GenericTimeSeriesVariableType{left, guarantee};
    const auto generic_right = bg::GenericTimeSeriesVariableType{right, guarantee};
    const auto &list =
        bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::ordinary>, 2, bg::TimeSeriesGuarantee::final>::type();
    const auto &value = bg::TS<bg::i64>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_left.match(list, type_map));
    REQUIRE(generic_right.match(value, type_map));

    CHECK(&generic_left.resolve(type_map) == &bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::final>, 2>::type());
    CHECK(&generic_right.resolve(type_map) == &value);
}

TEST_CASE("a deduplicated value time series requires equality",
          "[type_system][feature:type-system][series:value][scalar:opaque][guarantee:deduplicated]") {
    CHECK_THROWS_AS(
        bg::time_series_value_type(bg::cpp_scalar_type<bg::Opaque<int>>(), bg::TimeSeriesGuarantee::deduplicated),
        std::invalid_argument);
}

TEST_CASE("a time series variable ignores the guarantees captured separately",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated]"
          "[guarantee:final]") {
    auto variable = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"T"}};
    auto guarantee = bg::GuaranteeVariable{bg::util::symbol{"G"}};
    const auto generic = bg::GenericTimeSeriesVariableType{variable, guarantee};
    const auto &final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const auto &deduplicated = bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>::type();
    auto type_map = bg::TypeMap{};

    REQUIRE(generic.match(final, type_map));
    REQUIRE(generic.match(deduplicated, type_map));

    CHECK(&generic.resolve(type_map) == &deduplicated);
}

TEST_CASE("resolved time-series value types are reused", "[type_system][feature:type-system][guarantee:final]") {
    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();

    CHECK(&ts_final == &bg::TS<float, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("signature resolution uses reference target types",
          "[type_system][reference][feature:type-system][binding:reference-to-value]") {
    const auto &target = bg::TS<std::int64_t>::type();
    const auto &reference = bg::REF<bg::TS<std::int64_t>>::type();
    const auto value_input = bg::Signature{bg::util::symbol{"value_input"}, {{bg::util::symbol{"value"}, &target}}};
    const auto reference_input =
        bg::Signature{bg::util::symbol{"reference_input"}, {{bg::util::symbol{"value"}, &reference}}};
    const auto arguments = std::array<const bg::Type *, 1>{&reference};

    CHECK(value_input.resolve(arguments).parameters[0].type == &target);
    CHECK(reference_input.resolve(arguments).parameters[0].type == &reference);
}

TEST_CASE("a resolved parameter constrains a generic argument",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto signature = bg::Signature{
        bg::util::symbol{"fixed"}, {{bg::util::symbol{"value"}, &bg::TS<float>::type()}}};
    const auto arguments = std::array<const bg::Type *, 1>{&bg::TS_T::type()};

    const auto resolution = signature.resolve_with_type_map(arguments);

    CHECK(resolution.signature.parameters[0].type == &bg::TS<float>::type());
    CHECK(&bg::TS_T::type().resolve(resolution.type_map) == &bg::TS<float>::type());
}

TEST_CASE("resolved arguments constrain a shared parameter before generic arguments",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto signature = bg::Signature{
        bg::util::symbol{"same"},
        {{bg::util::symbol{"lhs"}, &bg::TS_T::type()}, {bg::util::symbol{"rhs"}, &bg::TS_T::type()}}};
    const auto arguments = std::array<const bg::Type *, 2>{&bg::TS_T::type(), &bg::TS<float>::type()};

    const auto resolution = signature.resolve_with_type_map(arguments);

    CHECK(resolution.signature.parameters[0].type == &bg::TS<float>::type());
    CHECK(resolution.signature.parameters[1].type == &bg::TS<float>::type());
}

TEST_CASE("signature resolution rejects mutually unconstrained argument and parameter variables",
          "[type_system][feature:type-system][declaration:generic]") {
    auto parameter_variable = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"PARAMETER"}};
    auto argument_variable = bg::TypeVariableType<bg::TimeSeriesType>{bg::util::symbol{"ARGUMENT"}};
    const auto parameter = bg::GenericTimeSeriesVariableType{
        parameter_variable, bg::TimeSeriesGuarantee::ordinary};
    const auto argument = bg::GenericTimeSeriesVariableType{
        argument_variable, bg::TimeSeriesGuarantee::ordinary};
    const auto signature = bg::Signature{
        bg::util::symbol{"unconstrained"}, {{bg::util::symbol{"value"}, &parameter}}};
    const auto arguments = std::array<const bg::Type *, 1>{&argument};

    CHECK_THROWS_AS(signature.resolve(arguments), bg::TypeResolutionError);
}

TEST_CASE("a signature resolves scalar parameters", "[type_system][feature:type-system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};

    bg::TypeVariableType<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&float_type, &ts_float}};
    bg::TypeVariableType<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&float_type, &ts_float}};

    const bg::Signature add{
        bg::util::symbol{"add"},
        {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
        ts_float,
        {{&guarantee, [](const bg::TypeMap &) -> bg::TypeMapValue { return bg::TimeSeriesGuarantee::const_; }}}};

    const std::array<const bg::Type *, 2> scalar_arguments{&float_type, &float_type};
    const auto scalar_add = add.resolve(scalar_arguments);

    CHECK(scalar_add.parameters[0].type->is_scalar());
    CHECK(scalar_add.parameters[1].type->is_scalar());
    CHECK(is_value_type(scalar_add.output, float_type, bg::TimeSeriesGuarantee::const_));
}

TEST_CASE("a signature resolves scalar and time-series parameters", "[type_system][feature:type-system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};
    bg::TypeVariableType<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&float_type, &ts_float}};
    bg::TypeVariableType<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&float_type, &ts_float}};
    const bg::Signature add{bg::util::symbol{"add"},
                            {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
                            ts_float};

    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const std::array<const bg::Type *, 2> mixed_arguments{&float_type, &ts_final};
    const auto mixed_add = add.resolve(mixed_arguments);

    CHECK(mixed_add.parameters[0].type->is_scalar());
    CHECK(mixed_add.parameters[1].type->is_time_series());
    CHECK(mixed_add.parameters[1].type == &ts_final);
    CHECK(is_value_type(mixed_add.output, float_type, bg::TimeSeriesGuarantee::final));
}

TEST_CASE("a shared guarantee variable resolves to the weakest input guarantee",
          "[type_system][feature:type-system][declaration:generic][guarantee:deduplicated]"
          "[guarantee:final]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};
    bg::TypeVariableType<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&ts_float}};
    bg::TypeVariableType<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&ts_float}};
    const bg::Signature add{bg::util::symbol{"add"},
                            {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
                            ts_float};

    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const auto &ts_deduplicated = bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>::type();
    const std::array<const bg::Type *, 2> series_arguments{&ts_final, &ts_deduplicated};
    const auto series_add = add.resolve(series_arguments);

    CHECK(series_add.parameters[0].type->is_time_series());
    CHECK(series_add.parameters[1].type->is_time_series());
    CHECK(is_value_type(series_add.output, float_type, bg::TimeSeriesGuarantee::deduplicated));
}

TEST_CASE("a guarantee variable enforces its lower bound",
          "[type_system][feature:type-system][declaration:generic][guarantee:ordinary]"
          "[guarantee:deduplicated][guarantee:final]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable stable_guarantee{bg::util::symbol{"Stable"}, bg::TimeSeriesGuarantee::deduplicated};
    bg::GenericTimeSeriesValueType stable_ts_float{float_type, stable_guarantee};
    bg::TypeVariableType<bg::Type> stable_type{bg::util::symbol{"StableType"}, {&stable_ts_float}};
    const bg::Signature stable_input{
        bg::util::symbol{"stable_input"}, {{bg::util::symbol{"value"}, &stable_type}}, stable_ts_float};

    const auto &ts_ordinary = bg::TS<float>::type();
    const std::array<const bg::Type *, 1> weak_argument{&ts_ordinary};

    try {
        static_cast<void>(stable_input.resolve(weak_argument));
        FAIL("ordinary guarantee satisfied a deduplicated lower bound");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
    }

    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const std::array<const bg::Type *, 1> stable_argument{&ts_final};
    const auto resolved_stable_input = stable_input.resolve(stable_argument);
    CHECK(is_value_type(resolved_stable_input.output, float_type, bg::TimeSeriesGuarantee::final));
}

TEST_CASE("repeated use of a type variable requires one resolved type",
          "[type_system][feature:type-system][declaration:generic]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();
    const auto &ts_ordinary = bg::TS<float>::type();

    bg::TypeVariableType<bg::Type> same_type{bg::util::symbol{"Same"}};
    const bg::Signature same_inputs{bg::util::symbol{"same_inputs"},
                                    {{bg::util::symbol{"first"}, &same_type}, {bg::util::symbol{"second"}, &same_type}},
                                    ts_ordinary};
    const std::array<const bg::Type *, 2> different_arguments{&float_type, &ts_final};

    try {
        static_cast<void>(same_inputs.resolve(different_arguments));
        FAIL("one type variable resolved to different argument types");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "second");
    }
}

TEST_CASE("a type parameter maps a time-series type variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const auto &ts_final = bg::TS<float, bg::TimeSeriesGuarantee::final>::type();

    bg::TypeVariableType<bg::TimeSeriesType> selected_type{bg::util::symbol{"Selected"}};
    bg::GenericTypeScalarType type_parameter{selected_type};
    const bg::Signature select_type{
        bg::util::symbol{"select_type"}, {{bg::util::symbol{"type"}, &type_parameter}}, selected_type};
    const bg::TypeScalarType &final_type_argument = bg::type_of(ts_final);
    const std::array<const bg::Type *, 1> final_type_arguments{&final_type_argument};
    const auto selected_final = select_type.resolve(final_type_arguments);

    CHECK(selected_final.parameters[0].type->is_scalar());
    CHECK(selected_final.parameters[0].type == &final_type_argument);
    CHECK(selected_final.output == &ts_final);
}

TEST_CASE("a type parameter maps a nested guarantee variable",
          "[type_system][feature:type-system][declaration:generic]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_deduplicated = bg::TS<float, bg::TimeSeriesGuarantee::deduplicated>::type();

    bg::GuaranteeVariable selected_guarantee{bg::util::symbol{"SelectedGuarantee"}};
    bg::GenericTimeSeriesValueType selected_ts_float{float_type, selected_guarantee};
    bg::GenericTypeScalarType constrained_type_parameter{selected_ts_float};
    const bg::Signature select_guarantee{bg::util::symbol{"select_guarantee"},
                                         {{bg::util::symbol{"type"}, &constrained_type_parameter}},
                                         selected_ts_float};
    const bg::TypeScalarType &deduplicated_type_argument = bg::type_of(ts_deduplicated);
    const std::array<const bg::Type *, 1> deduplicated_type_arguments{&deduplicated_type_argument};
    const auto selected_deduplicated = select_guarantee.resolve(deduplicated_type_arguments);

    CHECK(is_value_type(selected_deduplicated.output, float_type, bg::TimeSeriesGuarantee::deduplicated));
}

TEST_CASE("a time-series type parameter rejects a scalar type", "[type_system][feature:type-system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable selected_guarantee{bg::util::symbol{"SelectedGuarantee"}};
    bg::GenericTimeSeriesValueType selected_ts_float{float_type, selected_guarantee};
    bg::GenericTypeScalarType constrained_type_parameter{selected_ts_float};
    const bg::Signature select_guarantee{bg::util::symbol{"select_guarantee"},
                                         {{bg::util::symbol{"type"}, &constrained_type_parameter}},
                                         selected_ts_float};

    const bg::TypeScalarType &scalar_type_argument = bg::type_of(float_type);
    const std::array<const bg::Type *, 1> scalar_type_arguments{&scalar_type_argument};

    try {
        static_cast<void>(select_guarantee.resolve(scalar_type_arguments));
        FAIL("a scalar type satisfied a time-series type parameter");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "type");
    }
}

TEST_CASE("a generic scalar list resolves its item type", "[type_system][feature:type-system][declaration:generic]") {
    bg::TypeVariableType<bg::ScalarType> item{bg::util::symbol{"Item"}};
    const auto generic_list = bg::GenericListScalarType{item};
    const auto &float_list = bg::list_scalar_type(bg::float_type());
    auto type_map = bg::TypeMap{};

    REQUIRE(generic_list.match(float_list, type_map));
    CHECK(&generic_list.resolve(type_map) == &float_list);
}

TEST_CASE("generic set and dictionary types resolve their components",
          "[type_system][feature:type-system][declaration:generic]") {
    bg::TypeVariableType<bg::ScalarType> key{bg::util::symbol{"Key"}};
    bg::TypeVariableType<bg::ScalarType> value{bg::util::symbol{"Value"}};
    const auto generic_set = bg::GenericSetScalarType{key};
    const auto generic_dict = bg::GenericDictScalarType{key, value};
    const auto &resolved_set = bg::set_scalar_type(bg::string_type());
    const auto &resolved_dict = bg::dict_scalar_type(bg::string_type(), bg::int64_type());

    auto set_bindings = bg::TypeMap{};
    REQUIRE(generic_set.match(resolved_set, set_bindings));
    CHECK(&generic_set.resolve(set_bindings) == &resolved_set);

    auto dict_bindings = bg::TypeMap{};
    REQUIRE(generic_dict.match(resolved_dict, dict_bindings));
    CHECK(&generic_dict.resolve(dict_bindings) == &resolved_dict);
}

TEST_CASE("set items and dictionary keys require hashing", "[type_system][scalar][collection][feature:type-system]") {
    const auto &opaque = bg::cpp_scalar_type<bg::Opaque<int>>();

    CHECK_THROWS_AS(bg::set_scalar_type(opaque), std::invalid_argument);
    CHECK_THROWS_AS(bg::dict_scalar_type(opaque, bg::int64_type()), std::invalid_argument);
}

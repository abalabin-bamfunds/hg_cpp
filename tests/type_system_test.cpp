#include <bg/type/list_scalar_type.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <concepts>

namespace {

static_assert(std::derived_from<bg::GenericTimeSeriesValueType, bg::TimeSeriesValueType>);

[[nodiscard]] bool is_value_type(const bg::Type *type, const bg::ScalarType &value_type,
                                 bg::TimeSeriesGuarantee guarantee) {
    const auto *time_series = dynamic_cast<const bg::TimeSeriesValueType *>(type);

    return time_series != nullptr && &time_series->value_type() == &value_type &&
           time_series->guarantee().value() == guarantee;
}

} // namespace

TEST_CASE("evaluation time has a scalar graph type", "[type_system]") {
    CHECK(&bg::cpp_scalar_type<bg::EvaluationTime>() == &bg::evaluation_time_type());
    CHECK(&bg::cpp_scalar_type<bg::List<bg::EvaluationTime>>() == &bg::list_scalar_type(bg::evaluation_time_type()));
}

TEST_CASE("scalar graph types report equality and hashing", "[type_system]") {
    CHECK(bg::int64_type().supports_equality());
    CHECK(bg::int64_type().supports_hash());
    CHECK(bg::list_scalar_type(bg::int64_type()).supports_equality());
    CHECK(bg::list_scalar_type(bg::int64_type()).supports_hash());

    const auto &opaque = bg::cpp_scalar_type<bg::Opaque<int>>();
    CHECK_FALSE(opaque.supports_equality());
    CHECK_FALSE(opaque.supports_hash());
}

TEST_CASE("a generic time-series type retains its guarantee variable", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};

    REQUIRE_FALSE(ts_float.is_resolved());
    CHECK(&ts_float.value_type() == &float_type);
    CHECK(ts_float.guarantee().variable() == &guarantee);
}

TEST_CASE("resolved time-series value types are reused", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);

    CHECK(&ts_final == &bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final));
}

TEST_CASE("a signature resolves scalar parameters", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};

    bg::TypeVariable<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&float_type, &ts_float}};
    bg::TypeVariable<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&float_type, &ts_float}};

    const bg::Signature add{
        bg::util::symbol{"add"},
        {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
        ts_float,
        {{&guarantee, [](const bg::TypeMap &) -> bg::TypeBinding { return bg::TimeSeriesGuarantee::const_; }}}};

    const std::array<const bg::Type *, 2> scalar_arguments{&float_type, &float_type};
    const auto scalar_add = add.resolve(scalar_arguments);

    CHECK(scalar_add.parameters[0].type->is_scalar());
    CHECK(scalar_add.parameters[1].type->is_scalar());
    CHECK(is_value_type(scalar_add.output, float_type, bg::TimeSeriesGuarantee::const_));
}

TEST_CASE("a signature resolves scalar and time-series parameters", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};
    bg::TypeVariable<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&float_type, &ts_float}};
    bg::TypeVariable<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&float_type, &ts_float}};
    const bg::Signature add{bg::util::symbol{"add"},
                            {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
                            ts_float};

    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);
    const std::array<const bg::Type *, 2> mixed_arguments{&float_type, &ts_final};
    const auto mixed_add = add.resolve(mixed_arguments);

    CHECK(mixed_add.parameters[0].type->is_scalar());
    CHECK(mixed_add.parameters[1].type->is_time_series());
    CHECK(mixed_add.parameters[1].type == &ts_final);
    CHECK(is_value_type(mixed_add.output, float_type, bg::TimeSeriesGuarantee::final));
}

TEST_CASE("a shared guarantee variable resolves to the weakest input guarantee", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable guarantee{bg::util::symbol{"G"}};
    bg::GenericTimeSeriesValueType ts_float{float_type, guarantee};
    bg::TypeVariable<bg::Type> lhs_type{bg::util::symbol{"Lhs"}, {&ts_float}};
    bg::TypeVariable<bg::Type> rhs_type{bg::util::symbol{"Rhs"}, {&ts_float}};
    const bg::Signature add{bg::util::symbol{"add"},
                            {{bg::util::symbol{"lhs"}, &lhs_type}, {bg::util::symbol{"rhs"}, &rhs_type}},
                            ts_float};

    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);
    const auto &ts_deduplicated = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::deduplicated);
    const std::array<const bg::Type *, 2> series_arguments{&ts_final, &ts_deduplicated};
    const auto series_add = add.resolve(series_arguments);

    CHECK(series_add.parameters[0].type->is_time_series());
    CHECK(series_add.parameters[1].type->is_time_series());
    CHECK(is_value_type(series_add.output, float_type, bg::TimeSeriesGuarantee::deduplicated));
}

TEST_CASE("a guarantee variable enforces its lower bound", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();

    bg::GuaranteeVariable stable_guarantee{bg::util::symbol{"Stable"}, bg::TimeSeriesGuarantee::deduplicated};
    bg::GenericTimeSeriesValueType stable_ts_float{float_type, stable_guarantee};
    bg::TypeVariable<bg::Type> stable_type{bg::util::symbol{"StableType"}, {&stable_ts_float}};
    const bg::Signature stable_input{
        bg::util::symbol{"stable_input"}, {{bg::util::symbol{"value"}, &stable_type}}, stable_ts_float};

    const auto &ts_ordinary = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::ordinary);
    const std::array<const bg::Type *, 1> weak_argument{&ts_ordinary};

    try {
        static_cast<void>(stable_input.resolve(weak_argument));
        FAIL("ordinary guarantee satisfied a deduplicated lower bound");
    } catch (const bg::TypeResolutionError &error) {
        CHECK(error.code() == bg::TypeResolutionErrorCode::mismatch);
        CHECK(error.subject().text() == "value");
    }

    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);
    const std::array<const bg::Type *, 1> stable_argument{&ts_final};
    const auto resolved_stable_input = stable_input.resolve(stable_argument);
    CHECK(is_value_type(resolved_stable_input.output, float_type, bg::TimeSeriesGuarantee::final));
}

TEST_CASE("repeated use of a type variable requires one resolved type", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);
    const auto &ts_ordinary = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::ordinary);

    bg::TypeVariable<bg::Type> same_type{bg::util::symbol{"Same"}};
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

TEST_CASE("a type parameter binds a time-series type variable", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_final = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::final);

    bg::TypeVariable<bg::TimeSeriesType> selected_type{bg::util::symbol{"Selected"}};
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

TEST_CASE("a type parameter binds a nested guarantee variable", "[type_system]") {
    const bg::ScalarType &float_type = bg::float_type();
    const auto &ts_deduplicated = bg::time_series_value_type(float_type, bg::TimeSeriesGuarantee::deduplicated);

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

TEST_CASE("a time-series type parameter rejects a scalar type", "[type_system]") {
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

TEST_CASE("a generic scalar list resolves its item type", "[type_system]") {
    bg::TypeVariable<bg::ScalarType> item{bg::util::symbol{"Item"}};
    const auto generic_list = bg::GenericListScalarType{item};
    const auto &float_list = bg::list_scalar_type(bg::float_type());
    auto bindings = bg::TypeMap{};

    REQUIRE(generic_list.match(float_list, bindings));
    CHECK(&generic_list.resolve(bindings) == &float_list);
}

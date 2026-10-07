#include <bg/graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/scalar_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace bg::literals;

using RESOLVED_SCALAR = bg::ScalarVariable<"RESOLVED_SCALAR">;
using RESOLVED_NUMBER = bg::ScalarVariable<"RESOLVED_NUMBER", int, float>;
using DEPENDENT_KEY = bg::ScalarVariable<"DEPENDENT_KEY">;
using DEPENDENT_VALUE = bg::ScalarVariable<"DEPENDENT_VALUE">;
using CYCLIC_LEFT = bg::ScalarVariable<"CYCLIC_LEFT">;
using CYCLIC_RIGHT = bg::ScalarVariable<"CYCLIC_RIGHT">;
using RESOLVED_TIME_SERIES = bg::TimeSeriesVariable<"RESOLVED_TIME_SERIES">;
using NESTED_RESULT = bg::ScalarVariable<"NESTED_RESULT">;
using VARIADIC_RESULT = bg::ScalarVariable<"VARIADIC_RESULT">;
using UNMATCHED_FORM_VALUE = bg::ScalarVariable<"UNMATCHED_FORM_VALUE">;
using STATE_ONLY_VALUE = bg::ScalarVariable<"STATE_ONLY_VALUE", std::int64_t, float>;
using REQUIREMENT_ONLY_VALUE = bg::ScalarVariable<"REQUIREMENT_ONLY_VALUE", std::int64_t, float>;

template <template <typename, auto...> typename Field> struct ResolvedBundle {
    Field<bg::TS<bg::i64>> value;
};

template <template <typename, auto...> typename Field> struct UnmatchedFormBundle {
    Field<bg::TS<UNMATCHED_FORM_VALUE>> value;
};

inline constexpr auto RESOLVED_GUARANTEE = bg::guarantee_variable<"RESOLVED_GUARANTEE">;
using RESOLVED_GUARANTEE_DECLARATION = std::remove_cv_t<decltype(RESOLVED_GUARANTEE)>;
using SIZE_DECLARATION = std::remove_cv_t<decltype(bg::SIZE)>;

struct resolved_source_definition {
    int kind;
    bg::Out<bg::TS<RESOLVED_SCALAR>> output;

    inline static auto resolver_calls = 0;

    static const bg::ScalarType *resolve(RESOLVED_SCALAR, const bg::WiringArguments &arguments) {
        ++resolver_calls;
        const auto kind = bg::scalar_cast<std::int64_t>(*arguments.scalar("kind")).value();

        if (kind == 0) {
            return &bg::int64_type();
        }

        if (kind == 1) {
            return &bg::float_type();
        }

        return nullptr;
    }

    void evaluate() const {}
};

struct state_only_resolver_definition {
    bg::State<bg::List<STATE_ONLY_VALUE>> state;
    bg::Out<bg::TS<bg::i64>> output;

    inline static auto resolver_calls = 0;

    static const bg::ScalarType &resolve(STATE_ONLY_VALUE, const bg::WiringArguments &) {
        ++resolver_calls;
        return bg::int64_type();
    }

    void evaluate() const {}
};

struct invalid_state_only_resolver_definition {
    bg::In<bg::TS<bg::SCALAR>> input;
    bg::State<bg::List<STATE_ONLY_VALUE>> state;
    bg::Out<bg::TS<bg::i64>> output;

    inline static auto resolver_calls = 0;

    static const bg::ScalarType &resolve(STATE_ONLY_VALUE, const bg::WiringArguments &) {
        ++resolver_calls;
        return bg::string_type();
    }

    void evaluate() const {}
};

struct resolved_graph_definition {
    bg::wp<bg::TS_T> input;
    int kind;

    inline static auto resolver_calls = 0;

    static const bg::ScalarType *resolve(RESOLVED_SCALAR, const bg::WiringArguments &arguments) {
        ++resolver_calls;
        return bg::scalar_cast<std::int64_t>(*arguments.scalar("kind")).value() == 0 ? &bg::int64_type()
                                                                                      : &bg::float_type();
    }

    bg::wp<bg::TS<RESOLVED_SCALAR>> wire() const { return bg::wp<bg::TS<RESOLVED_SCALAR>>{input}; }
};

struct invalid_number_source_definition {
    bg::Out<bg::TS<RESOLVED_NUMBER>> output;

    static const bg::ScalarType &resolve(RESOLVED_NUMBER, const bg::WiringArguments &) { return bg::string_type(); }

    void evaluate() const {}
};

struct dependent_dictionary_source_definition {
    bg::Out<bg::TSD<DEPENDENT_KEY, bg::TS<DEPENDENT_VALUE>>> output;

    static const bg::ScalarType &resolve(DEPENDENT_KEY, const bg::WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(DEPENDENT_VALUE::type());
        return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
    }

    static const bg::ScalarType &resolve(DEPENDENT_VALUE, const bg::WiringArguments &) { return bg::float_type(); }

    void evaluate() const {}
};

struct collection_source_definition {
    bg::Out<bg::TSL<bg::TS<RESOLVED_SCALAR, RESOLVED_GUARANTEE>, bg::SIZE>> output;

    static const bg::ScalarType &resolve(RESOLVED_SCALAR, const bg::WiringArguments &) { return bg::int64_type(); }

    static bg::TimeSeriesGuarantee resolve(RESOLVED_GUARANTEE_DECLARATION, const bg::WiringArguments &) {
        return bg::TimeSeriesGuarantee::final;
    }

    static std::size_t resolve(SIZE_DECLARATION, const bg::WiringArguments &) { return 3; }

    void evaluate() const {}
};

struct resolved_bundle_source_definition {
    bg::Out<bg::TSB<bg::SCHEMA>> output;

    static const bg::BundleSchemaType &resolve(bg::schema_type<bg::SCHEMA>, const bg::WiringArguments &) {
        return bg::TSB<ResolvedBundle>::type().schema_type();
    }

    void evaluate() const {}
};

struct flat_positional_resolver_definition {
    std::optional<int> fixed;
    bg::In<bg::TSB<bg::SCHEMA>, bg::var_args> values;
    bg::Out<bg::TS<VARIADIC_RESULT>> output;

    inline static auto calls = 0;

    static const bg::ScalarType &resolve(VARIADIC_RESULT, const bg::WiringArguments &arguments) {
        ++calls;
        REQUIRE(arguments.size() == 2);
        CHECK_FALSE(arguments.present("fixed"));
        CHECK(arguments.argument_name(1).text() == "1");

        return bg::scalar_cast<std::int64_t>(*arguments.scalar(1)).value() == 7 ? bg::int64_type()
                                                                                : bg::float_type();
    }

    void evaluate() const {}
};

TEST_CASE("an aggregate resolver completes an output type",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    resolved_source_definition::resolver_calls = 0;

    auto operation = bg::operator_<auto(int)->bg::TS<float>>{"resolved_source", {"kind"}};
    operation.overload(bg::node<resolved_source_definition>());

    const auto resolve = [&](int kind) -> const bg::TimeSeriesType & {
        auto wiring = bg::GraphWiring{};
        auto context = bg::WiringContext{wiring};

        return operation(kind).type();
    };

    CHECK(&resolve(0) == &bg::TS<bg::i64>::type());
    CHECK(&resolve(0) == &bg::TS<bg::i64>::type());
    CHECK(resolved_source_definition::resolver_calls == 1);
    CHECK(&resolve(1) == &bg::TS<float>::type());
    CHECK(resolved_source_definition::resolver_calls == 2);

    try {
        (void)resolve(2);
        FAIL("expected an invalid resolver result");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::resolver);
    }
}

TEST_CASE("resolvers in unmatched union alternatives stay unused",
          "[feature:wiring][declaration:generic]") {
    using FORM = bg::Union<"FORM", bg::TS<bg::i64>, bg::TSB<UnmatchedFormBundle>>;

    auto calls = 0;
    auto resolvers = std::vector<bg::VariableResolver>{};
    resolvers.emplace_back(&UNMATCHED_FORM_VALUE::type(), [&](const bg::TypeMap &) -> bg::TypeMapValue {
        ++calls;
        return static_cast<const bg::Type *>(&bg::int64_type());
    });
    const auto signature = bg::Signature{
        bg::util::symbol{"consume"}, {{bg::util::symbol{"value"}, &FORM::type()}}, std::move(resolvers)};
    const auto arguments = std::array<const bg::Type *, 1>{&bg::TS<bg::i64>::type()};
    auto type_map = signature.match_arguments(arguments);
    const auto resolved = signature.complete_resolution(type_map);

    REQUIRE(resolved.parameters.size() == 1);
    CHECK(resolved.parameters.front().type == &bg::TS<bg::i64>::type());
    CHECK(calls == 0);
}

TEST_CASE("candidate metadata demands state-only resolver mappings",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    state_only_resolver_definition::resolver_calls = 0;

    auto operation = bg::operator_<auto() -> bg::TS<bg::i64>>{"state_source"};
    operation.overload(bg::node<state_only_resolver_definition>());

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TS<bg::i64>::type()); });

    CHECK(state_only_resolver_definition::resolver_calls == 1);
}

TEST_CASE("a failed state-only resolver rejects its candidate before fallback selection",
          "[feature:operator][feature:wiring][definition:aggregate][declaration:generic]") {
    using namespace bg::literals;

    invalid_state_only_resolver_definition::resolver_calls = 0;
    auto fallback_selected = false;
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS<bg::i64>>{"state_fallback", {"input"}};
    operation.overload(bg::node<invalid_state_only_resolver_definition>());
    operation.overload(bg::graph("name"_n = "fallback", [&](bg::wp<bg::TS<bg::i64>> input) {
        fallback_selected = true;
        return input;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    bg::testing::test_wiring([&] {
        CHECK(&operation(source()).type() == &bg::TS<bg::i64>::type());
        CHECK(fallback_selected);
    });

    CHECK(invalid_state_only_resolver_definition::resolver_calls == 1);
}

TEST_CASE("requirements demand auxiliary resolvers and their scalar reads participate in caching",
          "[feature:operator][feature:wiring][definition:function]") {
    auto resolver_calls = 0;
    auto requirement_calls = 0;
    auto operation = bg::operator_<void(std::int64_t, bg::TS<bg::i64>)>{"consume", {"kind", "input"}};
    operation.overload(bg::node(
        bg::eval = [](std::int64_t, bg::In<bg::TS<bg::i64>>) {},
        bg::resolve<REQUIREMENT_ONLY_VALUE> = [&](const bg::WiringArguments &arguments) -> const bg::ScalarType & {
            ++resolver_calls;
            return bg::scalar_cast<std::int64_t>(*arguments.scalar(0)).value() == 0 ? bg::int64_type()
                                                                                   : bg::float_type();
        },
        bg::require = [&](const bg::WiringArguments &arguments) {
            ++requirement_calls;
            const auto *mapping = arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            REQUIRE(mapping != nullptr);
            const auto *type = std::get_if<const bg::Type *>(mapping);
            REQUIRE(type != nullptr);
            return *type == &bg::int64_type() ? bg::accept : bg::reject("resolved value is not int64");
        }));
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    bg::testing::test_wiring([&] {
        const auto input = source();
        operation(0, input);
        operation(0, input);
        CHECK(resolver_calls == 1);
        CHECK(requirement_calls == 1);

        try {
            operation(1, input);
            FAIL("expected the requirement to reject the second resolved type");
        } catch (const bg::WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::requirement);
        }
    });

    CHECK(resolver_calls == 2);
    CHECK(requirement_calls == 2);
}

TEST_CASE("a requirement error remains a callback error after catching a resolver failure",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_<void()>{"requirement_error"};
    operation.overload(bg::node(
        bg::eval = [] {},
        bg::resolve<REQUIREMENT_ONLY_VALUE> = [](const bg::WiringArguments &) -> const bg::ScalarType & {
            return bg::string_type();
        },
        bg::require = [](const bg::WiringArguments &arguments) {
            try {
                (void)arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            } catch (const bg::TypeResolutionError &) {
                throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map,
                                              bg::util::symbol{"requirement"}, "requirement owns this error"};
            }

            return bg::accept;
        }));

    bg::testing::test_wiring([&] {
        try {
            operation();
            FAIL("expected the requirement callback to fail");
        } catch (const bg::WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::callback);
            CHECK(std::string{error.what()}.contains("requirement owns this error"));
        }
    });
}

TEST_CASE("an invalid resolver demanded by a requirement rejects its candidate",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    using namespace bg::literals;

    auto resolver_calls = 0;
    auto fallback_selected = false;
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS<bg::i64>>{"requirement_fallback", {"input"}};
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TS<bg::SCALAR>>, bg::Out<bg::TS<bg::i64>>) {},
        bg::resolve<REQUIREMENT_ONLY_VALUE> = [&](const bg::WiringArguments &) -> const bg::ScalarType & {
            ++resolver_calls;
            return bg::string_type();
        },
        bg::require = [](const bg::WiringArguments &arguments) {
            (void)arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            return bg::accept;
        }));
    operation.overload(bg::graph("name"_n = "fallback", [&](bg::wp<bg::TS<bg::i64>> input) {
        fallback_selected = true;
        return input;
    }));
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    bg::testing::test_wiring([&] {
        CHECK(&operation(source()).type() == &bg::TS<bg::i64>::type());
        CHECK(fallback_selected);
    });

    CHECK(resolver_calls == 1);
}

TEST_CASE("an aggregate graph resolver completes its output type",
          "[feature:wiring][definition:aggregate][declaration:generic]") {
    resolved_graph_definition::resolver_calls = 0;

    const auto resolved = bg::graph<resolved_graph_definition>();
    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    bg::testing::test_wiring([&] {
        CHECK(&resolved(integer_source(), 0).type() == &bg::TS<bg::i64>::type());
        CHECK(&resolved(float_source(), 1).type() == &bg::TS<float>::type());
    });

    CHECK(resolved_graph_definition::resolver_calls == 2);
}

TEST_CASE("resolvers see one flat positional argument sequence",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:aggregate]") {
    flat_positional_resolver_definition::calls = 0;

    auto operation = bg::operator_<auto(int, bg::TS_T) -> bg::TS<bg::i64>>{"resolve", {"fixed", "values"}};
    operation.overload(bg::node<flat_positional_resolver_definition>());
    const auto sender = bg::graph(
        "name"_n = "sender",
        [&](std::optional<int> fixed,
            bg::wp<bg::TSB<bg::SCHEMA>, bg::var_args> values) -> bg::wp<bg::TS<bg::i64>> {
            return operation(fixed, values).as<bg::TS<bg::i64>>();
        });

    bg::testing::test_wiring([&] {
        CHECK(&operation(std::optional<int>{}, std::optional<int>{}, 7).type() == &bg::TS<bg::i64>::type());
        CHECK(&sender(std::optional<int>{}, std::optional<int>{}, 7).type() == &bg::TS<bg::i64>::type());
    });

    CHECK(flat_positional_resolver_definition::calls == 2);
}

TEST_CASE("resolvers find direct and forwarded named captures",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:function]") {
    auto calls = 0;
    auto operation = bg::operator_<auto(bg::TS_T) -> bg::TS<bg::i64>>{"resolve", {"values"}};
    operation.overload(bg::node(
        bg::eval = [](bg::In<bg::TSB<bg::SCHEMA>, bg::var_kwargs>, bg::Out<bg::TS<VARIADIC_RESULT>>) {},
        bg::resolve<VARIADIC_RESULT> = [&](const bg::WiringArguments &arguments) -> const bg::ScalarType & {
            ++calls;
            REQUIRE(arguments.size() == 2);
            CHECK(arguments.argument_index("kind") == 0);
            CHECK(arguments.argument_index("value") == 1);

            return bg::scalar_cast<std::int64_t>(*arguments.scalar("kind")).value() == 0 ? bg::int64_type()
                                                                                          : bg::float_type();
        }));
    const auto sender = bg::graph(
        "name"_n = "sender",
        [&](bg::wp<bg::TSB<bg::SCHEMA>, bg::var_kwargs> values) -> bg::wp<bg::TS<bg::i64>> {
            return operation(values).as<bg::TS<bg::i64>>();
        });

    bg::testing::test_wiring([&] {
        CHECK(&operation("kind"_n = 0, "value"_n = 1).type() == &bg::TS<bg::i64>::type());
        CHECK(&sender("kind"_n = 0, "value"_n = 1).type() == &bg::TS<bg::i64>::type());
    });

    CHECK(calls == 2);
}

TEST_CASE("a callable resolver completes an output type", "[feature:operator][feature:wiring][definition:function]") {
    auto resolver_calls = 0;
    auto operation = bg::operator_<auto(int)->bg::TS<float>>{"resolved_source", {"kind"}};
    operation.overload(bg::node(
        bg::eval = [](int, bg::Out<bg::TS<RESOLVED_SCALAR>>) {},
        bg::resolve<RESOLVED_SCALAR> = [&](const bg::WiringArguments &arguments) -> const bg::ScalarType * {
            ++resolver_calls;
            return bg::scalar_cast<std::int64_t>(*arguments.scalar(0)).value() == 0 ? &bg::int64_type()
                                                                                    : &bg::float_type();
        }));

    bg::testing::test_wiring([&] {
        CHECK(&operation(0).type() == &bg::TS<bg::i64>::type());
        CHECK(&operation(0).type() == &bg::TS<bg::i64>::type());
        CHECK(&operation(1).type() == &bg::TS<float>::type());
    });

    CHECK(resolver_calls == 2);
}

TEST_CASE("an aggregate resolver cannot bypass a variable constraint",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_<auto()->bg::TS<float>>{"number_source"};
    operation.overload(bg::node<invalid_number_source_definition>());

    bg::testing::test_wiring([&] {
        try {
            (void)operation();
            FAIL("expected an incompatible resolver result");
        } catch (const bg::WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == bg::WiringFailureStage::resolver);
        }
    });
}

TEST_CASE("a callable resolver cannot bypass a variable constraint",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_<auto()->bg::TS<float>>{"number_source"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TS<RESOLVED_NUMBER>>) {},
        bg::resolve<RESOLVED_NUMBER> = [](const bg::WiringArguments &) -> const bg::ScalarType & {
            return bg::string_type();
        }));

    bg::testing::test_wiring([&] { CHECK_THROWS_AS(operation(), bg::WiringError); });
}

TEST_CASE("aggregate resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_<auto()->bg::TSD<bg::i64, bg::TS<float>>>{"dictionary_source"};
    operation.overload(bg::node<dependent_dictionary_source_definition>());

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TSD<float, bg::TS<float>>::type()); });
}

TEST_CASE("callable resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_<auto()->bg::TSD<bg::i64, bg::TS<float>>>{"dictionary_source"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSD<DEPENDENT_KEY, bg::TS<DEPENDENT_VALUE>>>) {},
        bg::resolve<DEPENDENT_KEY> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(DEPENDENT_VALUE::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        },
        bg::resolve<DEPENDENT_VALUE> = []() -> const bg::ScalarType & { return bg::int64_type(); }));

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TSD<bg::i64, bg::TS<bg::i64>>::type()); });
}

TEST_CASE("resolver dependency cycles reject an overload", "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_<auto()->bg::TSD<bg::i64, bg::TS<bg::i64>>>{"cyclic_source"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSD<CYCLIC_LEFT, bg::TS<CYCLIC_RIGHT>>>) {},
        bg::resolve<CYCLIC_LEFT> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(CYCLIC_RIGHT::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        },
        bg::resolve<CYCLIC_RIGHT> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(CYCLIC_LEFT::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        }));

    bg::testing::test_wiring([&] { CHECK_THROWS_AS(operation(), bg::WiringError); });
}

TEST_CASE("aggregate resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_<auto()->bg::TSL<bg::TS<bg::i64>, 3>>{"collection_source"};
    operation.overload(bg::node<collection_source_definition>());

    bg::testing::test_wiring(
        [&] { CHECK(&operation().type() == &bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 3>::type()); });
}

TEST_CASE("callable resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_<auto()->bg::TSL<bg::TS<bg::i64>, 2>>{"collection_source"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSL<bg::TS<RESOLVED_SCALAR, RESOLVED_GUARANTEE>, bg::SIZE>>) {},
        bg::resolve<RESOLVED_SCALAR> = []() -> const bg::ScalarType & { return bg::float_type(); },
        bg::resolve<RESOLVED_GUARANTEE_DECLARATION> = [] { return bg::TimeSeriesGuarantee::const_; },
        bg::resolve<SIZE_DECLARATION> = [] { return std::size_t{2}; }));

    bg::testing::test_wiring(
        [&] { CHECK(&operation().type() == &bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::const_>, 2>::type()); });
}

TEST_CASE("a callable resolver can resolve a time-series variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    auto operation = bg::operator_<auto()->bg::TS<float>>{"resolved_source"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<RESOLVED_TIME_SERIES>) {},
        bg::resolve<RESOLVED_TIME_SERIES> = []() -> const bg::TimeSeriesType & { return bg::TS<float>::type(); }));

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TS<float>::type()); });
}

TEST_CASE("an aggregate resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:aggregate][declaration:generic][series:tsb]") {
    auto operation = bg::operator_<auto()->bg::TSB<ResolvedBundle>>{"resolved_bundle"};
    operation.overload(bg::node<resolved_bundle_source_definition>());

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TSB<ResolvedBundle>::type()); });
}

TEST_CASE("a callable resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic][series:tsb]") {
    auto operation = bg::operator_<auto()->bg::TSB<ResolvedBundle>>{"resolved_bundle"};
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSB<bg::SCHEMA>>) {},
        bg::resolve<bg::schema_type<bg::SCHEMA>> = []() -> const bg::BundleSchemaType & {
            return bg::TSB<ResolvedBundle>::type().schema_type();
        }));

    bg::testing::test_wiring([&] { CHECK(&operation().type() == &bg::TSB<ResolvedBundle>::type()); });
}

TEST_CASE("nested graph resolution provides wiring arguments to resolvers",
          "[feature:wiring][feature:nested-graph][definition:function][shape:nested]") {
    auto resolver_calls = 0;
    auto resolvers = std::vector<bg::VariableResolver>{};
    auto resolve_result = [&](const bg::TypeMap &, const bg::WiringArguments &arguments) -> bg::TypeMapValue {
        ++resolver_calls;
        const auto kind = bg::scalar_cast<std::int64_t>(*arguments.scalar("kind")).value();
        return static_cast<const bg::Type *>(kind == 0 ? &bg::int64_type() : &bg::float_type());
    };
    resolvers.emplace_back(&NESTED_RESULT::type(), resolve_result);
    auto parameters = std::vector<bg::Parameter>{
        {bg::util::symbol{"kind"}, &bg::cpp_scalar_type<std::int64_t>()},
        {bg::util::symbol{"input"}, &bg::TS<bg::i64>::type()},
    };
    auto signature = bg::Signature{bg::util::symbol{"inner"}, std::move(parameters),
                                   bg::TS<NESTED_RESULT>::type(), std::move(resolvers)};
    auto inner = bg::GraphTemplate{std::move(signature),
                                   [](int, bg::wp<bg::TS<bg::i64>> input) { return input; }};
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    const auto run = bg::node([](bg::Out<bg::TS<bool>>) {});

    bg::testing::test_wiring(
        [&] { CHECK(&bg::nested(inner, run(), 0, source()).type() == &bg::REF<bg::TS<bg::i64>>::type()); });
    CHECK(resolver_calls == 1);
}

} // namespace

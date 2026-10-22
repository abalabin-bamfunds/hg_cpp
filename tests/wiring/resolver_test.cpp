#include <bg/graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/nested.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
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

using namespace bg;

namespace {

using namespace bg::literals;

using RESOLVED_SCALAR = ScalarVariable<"RESOLVED_SCALAR">;
using RESOLVED_NUMBER = ScalarVariable<"RESOLVED_NUMBER", int, float>;
using DEPENDENT_KEY = ScalarVariable<"DEPENDENT_KEY">;
using DEPENDENT_VALUE = ScalarVariable<"DEPENDENT_VALUE">;
using CYCLIC_LEFT = ScalarVariable<"CYCLIC_LEFT">;
using CYCLIC_RIGHT = ScalarVariable<"CYCLIC_RIGHT">;
using RESOLVED_TIME_SERIES = TimeSeriesVariable<"RESOLVED_TIME_SERIES">;
using NESTED_RESULT = ScalarVariable<"NESTED_RESULT">;
using VARIADIC_RESULT = ScalarVariable<"VARIADIC_RESULT">;
using UNMATCHED_FORM_VALUE = ScalarVariable<"UNMATCHED_FORM_VALUE">;
using STATE_ONLY_VALUE = ScalarVariable<"STATE_ONLY_VALUE", i64, float>;
using REQUIREMENT_ONLY_VALUE = ScalarVariable<"REQUIREMENT_ONLY_VALUE", i64, float>;

template <template <typename, auto...> typename Field> struct ResolvedBundle {
    Field<TS<i64>> value;
};

template <template <typename, auto...> typename Field> struct UnmatchedFormBundle {
    Field<TS<UNMATCHED_FORM_VALUE>> value;
};

inline constexpr auto RESOLVED_GUARANTEE = guarantee_variable<"RESOLVED_GUARANTEE">;
using RESOLVED_GUARANTEE_DECLARATION = std::remove_cv_t<decltype(RESOLVED_GUARANTEE)>;
using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;

struct resolved_source_definition {
    int kind;
    Out<TS<RESOLVED_SCALAR>> output;

    inline static auto resolver_calls = 0;

    static const ScalarType *resolve(RESOLVED_SCALAR, const WiringArguments &arguments) {
        ++resolver_calls;
        const auto kind = scalar_cast<i64>(*arguments.scalar("kind")).value();

        if (kind == 0) {
            return &int64_type();
        }

        if (kind == 1) {
            return &float_type();
        }

        return nullptr;
    }

    void evaluate() const {}
};

struct state_only_resolver_definition {
    State<List<STATE_ONLY_VALUE>> state;
    Out<TS<i64>> output;

    inline static auto resolver_calls = 0;

    static const ScalarType &resolve(STATE_ONLY_VALUE, const WiringArguments &) {
        ++resolver_calls;
        return int64_type();
    }

    void evaluate() const {}
};

struct invalid_state_only_resolver_definition {
    In<TS<SCALAR>> input;
    State<List<STATE_ONLY_VALUE>> state;
    Out<TS<i64>> output;

    inline static auto resolver_calls = 0;

    static const ScalarType &resolve(STATE_ONLY_VALUE, const WiringArguments &) {
        ++resolver_calls;
        return string_type();
    }

    void evaluate() const {}
};

struct resolved_graph_definition {
    wp<TS_T> input;
    int kind;

    inline static auto resolver_calls = 0;

    static const ScalarType *resolve(RESOLVED_SCALAR, const WiringArguments &arguments) {
        ++resolver_calls;
        return scalar_cast<i64>(*arguments.scalar("kind")).value() == 0 ? &int64_type()
                                                                                      : &float_type();
    }

    wp<TS<RESOLVED_SCALAR>> wire() const { return wp<TS<RESOLVED_SCALAR>>{input}; }
};

struct invalid_number_source_definition {
    Out<TS<RESOLVED_NUMBER>> output;

    static const ScalarType &resolve(RESOLVED_NUMBER, const WiringArguments &) { return string_type(); }

    void evaluate() const {}
};

struct dependent_dictionary_source_definition {
    Out<TSD<DEPENDENT_KEY, TS<DEPENDENT_VALUE>>> output;

    static const ScalarType &resolve(DEPENDENT_KEY, const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(DEPENDENT_VALUE::type());
        return static_cast<const ScalarType &>(**std::get_if<const Type *>(mapping));
    }

    static const ScalarType &resolve(DEPENDENT_VALUE, const WiringArguments &) { return float_type(); }

    void evaluate() const {}
};

struct collection_source_definition {
    Out<TSL<TS<RESOLVED_SCALAR, RESOLVED_GUARANTEE>, SIZE>> output;

    static const ScalarType &resolve(RESOLVED_SCALAR, const WiringArguments &) { return int64_type(); }

    static TimeSeriesGuarantee resolve(RESOLVED_GUARANTEE_DECLARATION, const WiringArguments &) {
        return TimeSeriesGuarantee::final;
    }

    static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &) { return 3; }

    void evaluate() const {}
};

struct resolved_bundle_source_definition {
    Out<TSB<SCHEMA>> output;

    static const SchemaType &resolve(schema_type<SCHEMA>, const WiringArguments &) {
        return TSB<ResolvedBundle>::type().schema_type();
    }

    void evaluate() const {}
};

struct flat_positional_resolver_definition {
    std::optional<int> fixed;
    In<TSB<SCHEMA>, var_args> values;
    Out<TS<VARIADIC_RESULT>> output;

    inline static auto calls = 0;

    static const ScalarType &resolve(VARIADIC_RESULT, const WiringArguments &arguments) {
        ++calls;
        REQUIRE(arguments.size() == 2);
        CHECK_FALSE(arguments.present("fixed"));
        CHECK(arguments.argument_name(1).text() == "1");

        return scalar_cast<i64>(*arguments.scalar(1)).value() == 7 ? int64_type()
                                                                                : float_type();
    }

    void evaluate() const {}
};

TEST_CASE("an aggregate resolver completes an output type",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    resolved_source_definition::resolver_calls = 0;

    auto operation = operator_<auto(int)->TS<float>>{"resolved_source", {"kind"}};
    operation.overload(node<resolved_source_definition>());

    auto wiring = GraphWiring{};
    auto context = WiringContext{wiring};
    const auto resolve = [&](int kind) -> const TimeSeriesType & {
        return operation(kind).type();
    };

    CHECK(&resolve(0) == &TS<i64>::type());
    CHECK(&resolve(0) == &TS<i64>::type());
    CHECK(resolved_source_definition::resolver_calls == 1);
    CHECK(&resolve(1) == &TS<float>::type());
    CHECK(resolved_source_definition::resolver_calls == 2);

    try {
        (void)resolve(2);
        FAIL("expected an invalid resolver result");
    } catch (const WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::resolver);
    }
}

TEST_CASE("resolvers in unmatched union alternatives stay unused",
          "[feature:wiring][declaration:generic]") {
    using FORM = Union<"FORM", TS<i64>, TSB<UnmatchedFormBundle>>;

    auto calls = 0;
    auto resolvers = std::vector<VariableResolver>{};
    resolvers.emplace_back(&UNMATCHED_FORM_VALUE::type(), [&](const TypeMap &) -> TypeMapValue {
        ++calls;
        return static_cast<const Type *>(&int64_type());
    });
    const auto signature = Signature{
        util::symbol{"consume"}, {{util::symbol{"value"}, &FORM::type()}}, std::move(resolvers)};
    const auto arguments = std::array<const Type *, 1>{&TS<i64>::type()};
    auto type_map = signature.match_arguments(arguments);
    const auto resolved = signature.complete_resolution(type_map);

    REQUIRE(resolved.parameters.size() == 1);
    CHECK(resolved.parameters.front().type == &TS<i64>::type());
    CHECK(calls == 0);
}

TEST_CASE("candidate metadata demands state-only resolver mappings",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    state_only_resolver_definition::resolver_calls = 0;

    auto operation = operator_<auto() -> TS<i64>>{"state_source"};
    operation.overload(node<state_only_resolver_definition>());

    testing::test_wiring([&] { CHECK(&operation().type() == &TS<i64>::type()); });

    CHECK(state_only_resolver_definition::resolver_calls == 1);
}

TEST_CASE("a failed state-only resolver rejects its candidate before fallback selection",
          "[feature:operator][feature:wiring][definition:aggregate][declaration:generic]") {
    using namespace bg::literals;

    invalid_state_only_resolver_definition::resolver_calls = 0;
    auto fallback_selected = false;
    auto operation = operator_<auto(TS_T) -> TS<i64>>{"state_fallback", {"input"}};
    operation.overload(node<invalid_state_only_resolver_definition>());
    operation.overload(graph("name"_n = "fallback", [&](wp<TS<i64>> input) {
        fallback_selected = true;
        return input;
    }));

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        CHECK(&operation(source()).type() == &TS<i64>::type());
        CHECK(fallback_selected);
    });

    CHECK(invalid_state_only_resolver_definition::resolver_calls == 1);
}

TEST_CASE("requirements demand auxiliary resolvers and their scalar reads participate in caching",
          "[feature:operator][feature:wiring][definition:function]") {
    auto resolver_calls = 0;
    auto requirement_calls = 0;
    auto operation = operator_<void(i64, TS<i64>)>{"consume", {"kind", "input"}};
    operation.overload(node(
        eval = [](i64, In<TS<i64>>) {},
        resolve<REQUIREMENT_ONLY_VALUE> = [&](const WiringArguments &arguments) -> const ScalarType & {
            ++resolver_calls;
            return scalar_cast<i64>(*arguments.scalar(0)).value() == 0 ? int64_type()
                                                                                   : float_type();
        },
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            const auto *mapping = arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            REQUIRE(mapping != nullptr);
            const auto *type = std::get_if<const Type *>(mapping);
            REQUIRE(type != nullptr);
            return *type == &int64_type() ? accept : reject("resolved value is not int64");
        }));

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        const auto input = source();
        operation(0, input);
        operation(0, input);
        CHECK(resolver_calls == 1);
        CHECK(requirement_calls == 1);

        try {
            operation(1, input);
            FAIL("expected the requirement to reject the second resolved type");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::requirement);
        }
    });

    CHECK(resolver_calls == 2);
    CHECK(requirement_calls == 2);
}

TEST_CASE("a requirement error remains a callback error after catching a resolver failure",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = operator_<void()>{"requirement_error"};
    operation.overload(node(
        eval = [] {},
        resolve<REQUIREMENT_ONLY_VALUE> = [](const WiringArguments &) -> const ScalarType & {
            return string_type();
        },
        require = [](const WiringArguments &arguments) {
            try {
                (void)arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            } catch (const TypeResolutionError &) {
                throw TypeResolutionError{TypeResolutionErrorCode::invalid_type_map,
                                              util::symbol{"requirement"}, "requirement owns this error"};
            }

            return accept;
        }));

    testing::test_wiring([&] {
        try {
            operation();
            FAIL("expected the requirement callback to fail");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::callback);
            CHECK(std::string{error.what()}.contains("requirement owns this error"));
        }
    });
}

TEST_CASE("an invalid resolver demanded by a requirement rejects its candidate",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    using namespace bg::literals;

    auto resolver_calls = 0;
    auto fallback_selected = false;
    auto operation = operator_<auto(TS_T) -> TS<i64>>{"requirement_fallback", {"input"}};
    operation.overload(node(
        eval = [](In<TS<SCALAR>>, Out<TS<i64>>) {},
        resolve<REQUIREMENT_ONLY_VALUE> = [&](const WiringArguments &) -> const ScalarType & {
            ++resolver_calls;
            return string_type();
        },
        require = [](const WiringArguments &arguments) {
            (void)arguments.type_map().find(REQUIREMENT_ONLY_VALUE::type());
            return accept;
        }));
    operation.overload(graph("name"_n = "fallback", [&](wp<TS<i64>> input) {
        fallback_selected = true;
        return input;
    }));

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        CHECK(&operation(source()).type() == &TS<i64>::type());
        CHECK(fallback_selected);
    });

    CHECK(resolver_calls == 1);
}

TEST_CASE("an aggregate graph resolver completes its output type",
          "[feature:wiring][definition:aggregate][declaration:generic]") {
    resolved_graph_definition::resolver_calls = 0;

    const auto resolved = graph<resolved_graph_definition>();

    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto float_source = node([](Out<TS<float>>) {});

    testing::test_wiring([&] {
        CHECK(&resolved(integer_source(), 0).type() == &TS<i64>::type());
        CHECK(&resolved(float_source(), 1).type() == &TS<float>::type());
    });

    CHECK(resolved_graph_definition::resolver_calls == 2);
}

TEST_CASE("resolvers see one flat positional argument sequence",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:aggregate]") {
    flat_positional_resolver_definition::calls = 0;

    auto operation = operator_<auto(int, TS_T) -> TS<i64>>{"resolve", {"fixed", "values"}};
    operation.overload(node<flat_positional_resolver_definition>());

    const auto sender = graph(
        "name"_n = "sender",
        [&](std::optional<int> fixed,
            wp<TSB<SCHEMA>, var_args> values) -> wp<TS<i64>> {
            return operation(fixed, values).as<TS<i64>>();
        });

    testing::test_wiring([&] {
        CHECK(&operation(std::optional<int>{}, std::optional<int>{}, 7).type() == &TS<i64>::type());
        CHECK(&sender(std::optional<int>{}, std::optional<int>{}, 7).type() == &TS<i64>::type());
    });

    CHECK(flat_positional_resolver_definition::calls == 2);
}

TEST_CASE("resolvers find direct and forwarded named captures",
          "[feature:operator][feature:wiring][feature:variadic-wiring][definition:function]") {
    auto calls = 0;
    auto operation = operator_<auto(TS_T) -> TS<i64>>{"resolve", {"values"}};
    operation.overload(node(
        eval = [](In<TSB<SCHEMA>, var_kwargs>, Out<TS<VARIADIC_RESULT>>) {},
        resolve<VARIADIC_RESULT> = [&](const WiringArguments &arguments) -> const ScalarType & {
            ++calls;
            REQUIRE(arguments.size() == 2);
            CHECK(arguments.argument_index("kind") == 0);
            CHECK(arguments.argument_index("value") == 1);

            return scalar_cast<i64>(*arguments.scalar("kind")).value() == 0 ? int64_type()
                                                                                          : float_type();
        }));

    const auto sender = graph(
        "name"_n = "sender",
        [&](wp<TSB<SCHEMA>, var_kwargs> values) -> wp<TS<i64>> {
            return operation(values).as<TS<i64>>();
        });

    testing::test_wiring([&] {
        CHECK(&operation("kind"_n = 0, "value"_n = 1).type() == &TS<i64>::type());
        CHECK(&sender("kind"_n = 0, "value"_n = 1).type() == &TS<i64>::type());
    });

    CHECK(calls == 2);
}

TEST_CASE("a callable resolver completes an output type", "[feature:operator][feature:wiring][definition:function]") {
    auto resolver_calls = 0;
    auto operation = operator_<auto(int)->TS<float>>{"resolved_source", {"kind"}};
    operation.overload(node(
        eval = [](int, Out<TS<RESOLVED_SCALAR>>) {},
        resolve<RESOLVED_SCALAR> = [&](const WiringArguments &arguments) -> const ScalarType * {
            ++resolver_calls;
            return scalar_cast<i64>(*arguments.scalar(0)).value() == 0 ? &int64_type()
                                                                                    : &float_type();
        }));

    testing::test_wiring([&] {
        CHECK(&operation(0).type() == &TS<i64>::type());
        CHECK(&operation(0).type() == &TS<i64>::type());
        CHECK(&operation(1).type() == &TS<float>::type());
    });

    CHECK(resolver_calls == 2);
}

TEST_CASE("an aggregate resolver cannot bypass a variable constraint",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = operator_<auto()->TS<float>>{"number_source"};
    operation.overload(node<invalid_number_source_definition>());

    testing::test_wiring([&] {
        try {
            (void)operation();
            FAIL("expected an incompatible resolver result");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::resolver);
        }
    });
}

TEST_CASE("a callable resolver cannot bypass a variable constraint",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = operator_<auto()->TS<float>>{"number_source"};
    operation.overload(node(
        eval = [](Out<TS<RESOLVED_NUMBER>>) {},
        resolve<RESOLVED_NUMBER> = [](const WiringArguments &) -> const ScalarType & {
            return string_type();
        }));

    testing::test_wiring([&] { CHECK_THROWS_AS(operation(), WiringError); });
}

TEST_CASE("a resolver-produced time-series type can apply a guarantee",
          "[feature:operator][feature:wiring][feature:type-system][declaration:generic]"
          "[scalar:opaque][guarantee:deduplicated]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using DEDUPLICATED_RESULT = RESOLVED_TIME_SERIES::with<TimeSeriesGuarantee::deduplicated>;
    (void)cpp_scalar_runtime_type<Opaque<int>>();
    const auto unsupported = [] {
        return node(
            eval = [](Out<DEDUPLICATED_RESULT>) {},
            resolve<RESOLVED_TIME_SERIES> = [](const WiringArguments &) -> const TimeSeriesType & {
                return time_series_value_type(cpp_scalar_type<Opaque<int>>(),
                                                  TimeSeriesGuarantee::ordinary)
                    .with_guarantee(TimeSeriesGuarantee::deduplicated);
            });
    };
    const auto &opaque_type = time_series_value_type(cpp_scalar_type<Opaque<int>>(),
                                                         TimeSeriesGuarantee::ordinary);

    const auto check_registration_order = [&](bool unsupported_first) {
        auto operation = operator_<auto() -> TS_T>{"deduplicated_output"};
        auto fallback_selected = false;

        const auto concrete = node(
            eval = [](Out<OPAQUE_SERIES>) {},
            require = [&](const WiringArguments &) {
                fallback_selected = true;
                return accept;
            },
            resolve<OPAQUE_SERIES> = []() -> const TimeSeriesType & {
                return time_series_value_type(cpp_scalar_type<Opaque<int>>(),
                                                  TimeSeriesGuarantee::ordinary);
            });
        if (unsupported_first) {
            operation.overload(unsupported());
            operation.overload(concrete);
        } else {
            operation.overload(concrete);
            operation.overload(unsupported());
        }

        testing::test_wiring([&] {
            CHECK(&operation().type() == &opaque_type);
            CHECK(fallback_selected);
        });
    };

    check_registration_order(true);
    check_registration_order(false);

    auto operation = operator_<auto() -> TS_T>{"deduplicated_output_without_fallback"};
    operation.overload(unsupported());
    testing::test_wiring([&] {
        try {
            (void)operation();
            FAIL("expected no applicable overload");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            REQUIRE(error.diagnostic()->candidates.size() == 1);
            CHECK(error.diagnostic()->candidates.front().stage == WiringFailureStage::resolver);
            CHECK(std::string{error.what()}.contains("requires a scalar type that supports equality"));
        }
    });
}

TEST_CASE("a generic deduplicated output rejects an unsupported scalar before fallback selection",
          "[feature:operator][feature:wiring][declaration:generic][scalar:opaque]"
          "[guarantee:deduplicated]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    using VALUE = ScalarVariable<"DEDUPLICATED_OUTPUT_VALUE">;
    (void)cpp_scalar_runtime_type<Opaque<int>>();

    const auto source = node(
        eval = [](Out<OPAQUE_SERIES>) {},
        resolve<OPAQUE_SERIES> = []() -> const TimeSeriesType & {
            return time_series_value_type(cpp_scalar_type<Opaque<int>>(),
                                              TimeSeriesGuarantee::ordinary);
        });

    const auto generic = node(eval = [](In<TS<VALUE>>,
                                                Out<TS<VALUE, TimeSeriesGuarantee::deduplicated>>) {});
    const auto &opaque_type = time_series_value_type(cpp_scalar_type<Opaque<int>>(),
                                                         TimeSeriesGuarantee::ordinary);

    const auto check_registration_order = [&](bool generic_first) {
        auto operation = operator_<auto(TS_T) -> TS_T>{"generic_deduplicated_output", {"input"}};
        auto fallback_selected = false;

        const auto fallback = graph(
            "name"_n = "concrete_fallback",
            [&](wp<OPAQUE_SERIES> input) {
                fallback_selected = true;
                return input;
            });

        if (generic_first) {
            operation.overload(generic);
            operation.overload(fallback);
        } else {
            operation.overload(fallback);
            operation.overload(generic);
        }

        testing::test_wiring([&] {
            CHECK(&operation(source()).type() == &opaque_type);
            CHECK(fallback_selected);
        });
    };

    check_registration_order(true);
    check_registration_order(false);
}

TEST_CASE("a resolver callback invalid_argument remains a fatal wiring error",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    auto operation = operator_<auto() -> TS<float>>{"resolver_callback_error"};

    const auto concrete = node([](Out<TS<float>>) {});
    operation.overload(graph("name"_n = "concrete_fallback", [concrete] { return concrete(); }));
    operation.overload(node(
        eval = [](Out<TS<RESOLVED_SCALAR>>) {},
        resolve<RESOLVED_SCALAR> = [](const WiringArguments &) -> const ScalarType & {
            throw std::invalid_argument{"resolver callback failure"};
        }));

    testing::test_wiring([&] {
        try {
            (void)operation();
            FAIL("expected a resolver callback failure");
        } catch (const WiringError &error) {
            REQUIRE(error.diagnostic());
            auto callback_failure_found = false;
            for (const auto &candidate : error.diagnostic()->candidates) {
                callback_failure_found |= candidate.stage == WiringFailureStage::callback;
            }
            CHECK(callback_failure_found);
            CHECK(std::string{error.what()}.contains("resolver callback failure"));
        }
    });
}

TEST_CASE("generic identity overloads accept no-equality final and const values",
          "[feature:operator][feature:wiring][declaration:generic][scalar:opaque]"
          "[guarantee:final][guarantee:const]") {
    using OPAQUE_SERIES = TimeSeriesVariable<"OPAQUE_SERIES">;
    const auto &opaque = cpp_scalar_type<Opaque<int>>();
    (void)cpp_scalar_runtime_type<Opaque<int>>();
    const auto make_source = [](TimeSeriesGuarantee guarantee) {
        return node(
            eval = [](Out<OPAQUE_SERIES>) {},
            resolve<OPAQUE_SERIES> = [guarantee]() -> const TimeSeriesType & {
                return time_series_value_type(cpp_scalar_type<Opaque<int>>(), guarantee);
            });
    };
    auto operation = operator_<auto(TS_T) -> TS_T>{"opaque_identity", {"value"}};
    auto generic_selected = 0;
    auto deduplicated_selected = 0;
    operation.overload(node(
        eval = [](In<TS_T::with<TimeSeriesGuarantee::deduplicated>>,
                      Out<TS_T::with<TimeSeriesGuarantee::deduplicated>>) {},
        require = [&](const WiringArguments &) {
            ++deduplicated_selected;
            return accept;
        }));
    operation.overload(graph("name"_n = "identity", [&](wp<TS_T> value) {
        ++generic_selected;
        return value;
    }));
    const auto final_source = make_source(TimeSeriesGuarantee::final);
    const auto const_source = make_source(TimeSeriesGuarantee::const_);
    const auto &final_type = time_series_value_type(opaque, TimeSeriesGuarantee::final);
    const auto &const_type = time_series_value_type(opaque, TimeSeriesGuarantee::const_);

    testing::test_wiring([&] {
        CHECK(&operation(final_source()).type() == &final_type);
        CHECK(&operation(const_source()).type() == &const_type);
    });

    CHECK(generic_selected == 2);
    CHECK(deduplicated_selected == 0);
}

TEST_CASE("aggregate resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = operator_<auto()->TSD<i64, TS<float>>>{"dictionary_source"};
    operation.overload(node<dependent_dictionary_source_definition>());

    testing::test_wiring([&] { CHECK(&operation().type() == &TSD<float, TS<float>>::type()); });
}

TEST_CASE("callable resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = operator_<auto()->TSD<i64, TS<float>>>{"dictionary_source"};
    operation.overload(node(
        eval = [](Out<TSD<DEPENDENT_KEY, TS<DEPENDENT_VALUE>>>) {},
        resolve<DEPENDENT_KEY> = [](const TypeMap &type_map) -> const ScalarType & {
            const auto *mapping = type_map.find(DEPENDENT_VALUE::type());
            return static_cast<const ScalarType &>(**std::get_if<const Type *>(mapping));
        },
        resolve<DEPENDENT_VALUE> = []() -> const ScalarType & { return int64_type(); }));

    testing::test_wiring([&] { CHECK(&operation().type() == &TSD<i64, TS<i64>>::type()); });
}

TEST_CASE("resolver dependency cycles reject an overload", "[feature:operator][feature:wiring][definition:function]") {
    auto operation = operator_<auto()->TSD<i64, TS<i64>>>{"cyclic_source"};
    operation.overload(node(
        eval = [](Out<TSD<CYCLIC_LEFT, TS<CYCLIC_RIGHT>>>) {},
        resolve<CYCLIC_LEFT> = [](const TypeMap &type_map) -> const ScalarType & {
            const auto *mapping = type_map.find(CYCLIC_RIGHT::type());
            return static_cast<const ScalarType &>(**std::get_if<const Type *>(mapping));
        },
        resolve<CYCLIC_RIGHT> = [](const TypeMap &type_map) -> const ScalarType & {
            const auto *mapping = type_map.find(CYCLIC_LEFT::type());
            return static_cast<const ScalarType &>(**std::get_if<const Type *>(mapping));
        }));

    testing::test_wiring([&] { CHECK_THROWS_AS(operation(), WiringError); });
}

TEST_CASE("aggregate resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = operator_<auto()->TSL<TS<i64>, 3>>{"collection_source"};
    operation.overload(node<collection_source_definition>());

    testing::test_wiring(
        [&] { CHECK(&operation().type() == &TSL<TS<i64, TimeSeriesGuarantee::final>, 3>::type()); });
}

TEST_CASE("callable resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = operator_<auto()->TSL<TS<i64>, 2>>{"collection_source"};
    operation.overload(node(
        eval = [](Out<TSL<TS<RESOLVED_SCALAR, RESOLVED_GUARANTEE>, SIZE>>) {},
        resolve<RESOLVED_SCALAR> = []() -> const ScalarType & { return float_type(); },
        resolve<RESOLVED_GUARANTEE_DECLARATION> = [] { return TimeSeriesGuarantee::const_; },
        resolve<SIZE_DECLARATION> = [] { return std::size_t{2}; }));

    testing::test_wiring(
        [&] { CHECK(&operation().type() == &TSL<TS<float, TimeSeriesGuarantee::const_>, 2>::type()); });
}

TEST_CASE("a callable resolver can resolve a time-series variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    auto operation = operator_<auto()->TS<float>>{"resolved_source"};
    operation.overload(node(
        eval = [](Out<RESOLVED_TIME_SERIES>) {},
        resolve<RESOLVED_TIME_SERIES> = []() -> const TimeSeriesType & { return TS<float>::type(); }));

    testing::test_wiring([&] { CHECK(&operation().type() == &TS<float>::type()); });
}

TEST_CASE("an aggregate resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:aggregate][declaration:generic][series:tsb]") {
    auto operation = operator_<auto()->TSB<ResolvedBundle>>{"resolved_bundle"};
    operation.overload(node<resolved_bundle_source_definition>());

    testing::test_wiring([&] { CHECK(&operation().type() == &TSB<ResolvedBundle>::type()); });
}

TEST_CASE("a callable resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic][series:tsb]") {
    auto operation = operator_<auto()->TSB<ResolvedBundle>>{"resolved_bundle"};
    operation.overload(node(
        eval = [](Out<TSB<SCHEMA>>) {},
        resolve<schema_type<SCHEMA>> = []() -> const SchemaType & {
            return TSB<ResolvedBundle>::type().schema_type();
        }));

    testing::test_wiring([&] { CHECK(&operation().type() == &TSB<ResolvedBundle>::type()); });
}

TEST_CASE("nested graph resolution provides wiring arguments to resolvers",
          "[feature:wiring][feature:nested-graph][definition:function][shape:nested]") {
    auto resolver_calls = 0;
    auto resolvers = std::vector<VariableResolver>{};
    auto resolve_result = [&](const TypeMap &, const WiringArguments &arguments) -> TypeMapValue {
        ++resolver_calls;
        const auto kind = scalar_cast<i64>(*arguments.scalar("kind")).value();
        return static_cast<const Type *>(kind == 0 ? &int64_type() : &float_type());
    };
    resolvers.emplace_back(&NESTED_RESULT::type(), resolve_result);
    auto parameters = std::vector<Parameter>{
        {util::symbol{"kind"}, &cpp_scalar_type<i64>()},
        {util::symbol{"input"}, &TS<i64>::type()},
    };
    auto signature = Signature{util::symbol{"inner"}, std::move(parameters),
                                   TS<NESTED_RESULT>::type(), std::move(resolvers)};
    auto inner = GraphTemplate{std::move(signature),
                                   [](int, wp<TS<i64>> input) { return input; }};

    const auto source = node([](Out<TS<i64>>) {});

    const auto run = node([](Out<TS<bool>>) {});

    testing::test_wiring(
        [&] {
            CHECK(&nested(inner, run(), 0, source()).type() ==
                  &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
        });
    CHECK(resolver_calls == 1);
}

} // namespace

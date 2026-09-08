#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/time_series/tsb/tsb_type.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/scalar_variable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <type_traits>
#include <variant>

namespace {

using ResolvedScalar = bg::ScalarVariable<"ResolvedScalar">;
using ResolvedNumber = bg::ScalarVariable<"ResolvedNumber", int, float>;
using DependentKey = bg::ScalarVariable<"DependentKey">;
using DependentValue = bg::ScalarVariable<"DependentValue">;
using CyclicLeft = bg::ScalarVariable<"CyclicLeft">;
using CyclicRight = bg::ScalarVariable<"CyclicRight">;
using ResolvedTimeSeries = bg::TimeSeriesVariable<"ResolvedTimeSeries">;

template <template <typename> typename Field> struct ResolvedBundle {
    Field<bg::TS<int>> value;
};

inline constexpr auto ResolvedGuarantee = bg::guarantee_variable<"ResolvedGuarantee">;
using ResolvedGuaranteeDeclaration = std::remove_cv_t<decltype(ResolvedGuarantee)>;
using ResolvedSizeDeclaration = std::remove_cv_t<decltype(bg::SIZE)>;

struct resolved_source_definition {
    int kind;
    bg::Out<bg::TS<ResolvedScalar>> output;

    inline static auto resolver_calls = 0;

    static const bg::ScalarType *resolve(ResolvedScalar, const bg::WiringArguments &arguments) {
        ++resolver_calls;
        const auto kind = bg::scalar_cast<int>(*arguments.scalar("kind")).value();

        if (kind == 0) {
            return &bg::int_type();
        }

        if (kind == 1) {
            return &bg::float_type();
        }

        return nullptr;
    }

    void evaluate() const {}
};

struct invalid_number_source_definition {
    bg::Out<bg::TS<ResolvedNumber>> output;

    static const bg::ScalarType &resolve(ResolvedNumber, const bg::WiringArguments &) { return bg::string_type(); }

    void evaluate() const {}
};

struct dependent_dictionary_source_definition {
    bg::Out<bg::TSD<DependentKey, bg::TS<DependentValue>>> output;

    static const bg::ScalarType &resolve(DependentKey, const bg::WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(DependentValue::type());
        return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
    }

    static const bg::ScalarType &resolve(DependentValue, const bg::WiringArguments &) { return bg::float_type(); }

    void evaluate() const {}
};

struct collection_source_definition {
    bg::Out<bg::TSL<bg::TS<ResolvedScalar, ResolvedGuarantee>, bg::SIZE>> output;

    static const bg::ScalarType &resolve(ResolvedScalar, const bg::WiringArguments &) { return bg::int_type(); }

    static bg::TimeSeriesGuarantee resolve(ResolvedGuaranteeDeclaration, const bg::WiringArguments &) {
        return bg::TimeSeriesGuarantee::final;
    }

    static std::size_t resolve(ResolvedSizeDeclaration, const bg::WiringArguments &) { return 3; }

    void evaluate() const {}
};

struct resolved_bundle_source_definition {
    bg::Out<bg::TSB<bg::SCHEMA>> output;

    static const bg::BundleSchemaType &resolve(bg::schema_type<bg::SCHEMA>, const bg::WiringArguments &) {
        return bg::TSB<ResolvedBundle>::type().schema_type();
    }

    void evaluate() const {}
};

TEST_CASE("an aggregate resolver completes an output type",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    resolved_source_definition::resolver_calls = 0;

    auto operation = bg::operator_("resolved_source", [](int, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node<resolved_source_definition>());

    const auto resolve = [&](int kind) -> const bg::TimeSeriesType & {
        auto wiring = bg::GraphWiring{};
        auto context = bg::WiringContext{wiring};

        return operation(kind)->type();
    };

    CHECK(&resolve(0) == &bg::TS<int>::type());
    CHECK(&resolve(0) == &bg::TS<int>::type());
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

TEST_CASE("a callable resolver completes an output type", "[feature:operator][feature:wiring][definition:function]") {
    auto resolver_calls = 0;
    auto operation = bg::operator_("resolved_source", [](int, bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node(
        bg::eval = [](int, bg::Out<bg::TS<ResolvedScalar>>) {},
        bg::resolve<ResolvedScalar> = [&](const bg::WiringArguments &arguments) -> const bg::ScalarType * {
            ++resolver_calls;
            return bg::scalar_cast<int>(*arguments.scalar(0)).value() == 0 ? &bg::int_type() : &bg::float_type();
        }));

    bg::testing::test_wiring([&] {
        CHECK(&operation(0)->type() == &bg::TS<int>::type());
        CHECK(&operation(0)->type() == &bg::TS<int>::type());
        CHECK(&operation(1)->type() == &bg::TS<float>::type());
    });

    CHECK(resolver_calls == 2);
}

TEST_CASE("an aggregate resolver cannot bypass a variable constraint",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_("number_source", [](bg::Out<bg::TS<float>>) {});
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
    auto operation = bg::operator_("number_source", [](bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TS<ResolvedNumber>>) {},
        bg::resolve<ResolvedNumber> = [](const bg::WiringArguments &) -> const bg::ScalarType & {
            return bg::string_type();
        }));

    bg::testing::test_wiring([&] { CHECK_THROWS_AS(operation(), bg::WiringError); });
}

TEST_CASE("aggregate resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_("dictionary_source", [](bg::Out<bg::TSD<int, bg::TS<float>>>) {});
    operation.overload(bg::node<dependent_dictionary_source_definition>());

    bg::testing::test_wiring([&] { CHECK(&operation()->type() == &bg::TSD<float, bg::TS<float>>::type()); });
}

TEST_CASE("callable resolvers resolve dependencies on demand",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_("dictionary_source", [](bg::Out<bg::TSD<int, bg::TS<float>>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSD<DependentKey, bg::TS<DependentValue>>>) {},
        bg::resolve<DependentKey> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(DependentValue::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        },
        bg::resolve<DependentValue> = []() -> const bg::ScalarType & { return bg::int_type(); }));

    bg::testing::test_wiring([&] { CHECK(&operation()->type() == &bg::TSD<int, bg::TS<int>>::type()); });
}

TEST_CASE("resolver dependency cycles reject an overload", "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_("cyclic_source", [](bg::Out<bg::TSD<int, bg::TS<int>>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSD<CyclicLeft, bg::TS<CyclicRight>>>) {},
        bg::resolve<CyclicLeft> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(CyclicRight::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        },
        bg::resolve<CyclicRight> = [](const bg::TypeMap &type_map) -> const bg::ScalarType & {
            const auto *mapping = type_map.find(CyclicLeft::type());
            return static_cast<const bg::ScalarType &>(**std::get_if<const bg::Type *>(mapping));
        }));

    bg::testing::test_wiring([&] { CHECK_THROWS_AS(operation(), bg::WiringError); });
}

TEST_CASE("aggregate resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:aggregate]") {
    auto operation = bg::operator_("collection_source", [](bg::Out<bg::TSL<bg::TS<int>, 3>>) {});
    operation.overload(bg::node<collection_source_definition>());

    bg::testing::test_wiring(
        [&] { CHECK(&operation()->type() == &bg::TSL<bg::TS<int, bg::TimeSeriesGuarantee::final>, 3>::type()); });
}

TEST_CASE("callable resolver discovery follows nested type traits",
          "[feature:operator][feature:wiring][definition:function]") {
    auto operation = bg::operator_("collection_source", [](bg::Out<bg::TSL<bg::TS<int>, 2>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSL<bg::TS<ResolvedScalar, ResolvedGuarantee>, bg::SIZE>>) {},
        bg::resolve<ResolvedScalar> = []() -> const bg::ScalarType & { return bg::float_type(); },
        bg::resolve<ResolvedGuaranteeDeclaration> = [] { return bg::TimeSeriesGuarantee::const_; },
        bg::resolve<ResolvedSizeDeclaration> = [] { return std::size_t{2}; }));

    bg::testing::test_wiring(
        [&] { CHECK(&operation()->type() == &bg::TSL<bg::TS<float, bg::TimeSeriesGuarantee::const_>, 2>::type()); });
}

TEST_CASE("a callable resolver can resolve a time-series variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic]") {
    auto operation = bg::operator_("resolved_source", [](bg::Out<bg::TS<float>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<ResolvedTimeSeries>) {},
        bg::resolve<ResolvedTimeSeries> = []() -> const bg::TimeSeriesType & { return bg::TS<float>::type(); }));

    bg::testing::test_wiring([&] { CHECK(&operation()->type() == &bg::TS<float>::type()); });
}

TEST_CASE("an aggregate resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:aggregate][declaration:generic][series:tsb]") {
    auto operation = bg::operator_("resolved_bundle", [](bg::Out<bg::TSB<ResolvedBundle>>) {});
    operation.overload(bg::node<resolved_bundle_source_definition>());

    bg::testing::test_wiring([&] { CHECK(&operation()->type() == &bg::TSB<ResolvedBundle>::type()); });
}

TEST_CASE("a callable resolver can resolve a bundle schema variable",
          "[feature:operator][feature:wiring][definition:function][declaration:generic][series:tsb]") {
    auto operation = bg::operator_("resolved_bundle", [](bg::Out<bg::TSB<ResolvedBundle>>) {});
    operation.overload(bg::node(
        bg::eval = [](bg::Out<bg::TSB<bg::SCHEMA>>) {},
        bg::resolve<bg::schema_type<bg::SCHEMA>> = []() -> const bg::BundleSchemaType & {
            return bg::TSB<ResolvedBundle>::type().schema_type();
        }));

    bg::testing::test_wiring([&] { CHECK(&operation()->type() == &bg::TSB<ResolvedBundle>::type()); });
}

} // namespace

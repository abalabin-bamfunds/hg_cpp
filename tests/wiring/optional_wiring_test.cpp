#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

using namespace bg;
using namespace bg::literals;

namespace {

template <typename T> concept has_optional_wrapper = requires(T value) { opt(value); };
template <typename Source, typename Target> concept can_reannotate = requires(Source value) { Target{value}; };

} // namespace

TEST_CASE("opt preserves optionality through typed views and explicit required annotations",
          "[feature:wiring][feature:operator][validity:optional][series:value]") {
    static_assert(!has_optional_wrapper<i64>);

    auto selected = std::string{};
    auto optional_only = operator_<auto(TS_T)->TS_T>{"optional_only", {"value"}};
    optional_only.overload(graph("name"_n = "optional", [&](wp<TS<i64>, optional> value) {
        selected = "optional";
        return value;
    }));

    const auto source = node([](Out<TS<i64>>) {});
    testing::test_wiring([&] {
        const auto value = source();
        const auto required_point = wp<TS<i64>, required>{value};
        const auto optional_point = opt(required_point);
        const auto typed = optional_point.as<TS<i64>>();
        const auto repeated = opt(typed);

        static_assert(std::is_same_v<std::remove_cvref_t<decltype(optional_point)>, wp<TS<i64>, optional>>);
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(typed)>, wp<TS<i64>, optional>>);
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(repeated)>, wp<TS<i64>, optional>>);

        (void)optional_only(value);
        CHECK(selected == "optional");

        (void)optional_only(repeated);
        CHECK(selected == "optional");

        (void)optional_only(opt(WiringArgument{value}));
        CHECK(selected == "optional");
    });
}

TEST_CASE("opt keeps its qualifier across graph boundaries",
          "[feature:wiring][feature:operator][validity:optional][series:value][shape:nested]") {
    const auto optional_input = node([](In<TS<i64>, optional>) {});

    const auto required_boundary = graph("name"_n = "required_boundary", [&](wp<TS<i64>> value) {
        optional_input(value);
    });

    const auto optional_boundary = graph("name"_n = "optional_boundary", [&](wp<TS<i64>, optional> value) {
        optional_input(value);
    });

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] { optional_boundary(opt(source())); });
    CHECK_THROWS_AS(testing::test_wiring([&] { required_boundary(opt(source())); }), TypeResolutionError);
}

TEST_CASE("valid_items annotations compose with opt and typed views",
          "[feature:wiring][feature:operator][validity:optional][series:tsd]") {
    static_assert(sizeof(wp<TSD<i64, TS<i64>>, valid_items>) == sizeof(WiringPoint));
    static_assert(wp<TSD<i64, TS<i64>>, valid_items>::annotations.valid_items);
    static_assert(wp<TSD<i64, TS<i64>>, optional, valid_items>::annotations.optional);
    static_assert(wp<TSD<i64, TS<i64>>, optional, valid_items>::annotations.valid_items);
    static_assert(can_reannotate<wp<TSD<i64, TS<i64>>, valid_items>, wp<TSD<i64, TS<i64>>, valid_items>>);
    static_assert(can_reannotate<wp<TSD<i64, TS<i64>>, valid_items>,
                                 wp<TSD<i64, TS<i64>>, optional, valid_items>>);
    static_assert(!can_reannotate<wp<TSD<i64, TS<i64>>, valid_items>, wp<TSD<i64, TS<i64>>>>);
    static_assert(!can_reannotate<wp<TSD<i64, TS<i64>>, optional, valid_items>,
                                  wp<TSD<i64, TS<i64>>, valid_items>>);

    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto optional_input = node([](In<TSD<i64, TS<i64>>, optional, valid_items>) {});

    testing::test_wiring([&] {
        const auto value = source();
        const auto valid = bg::valid_items(value);
        const auto optional_valid = opt(valid);
        const auto valid_optional = bg::valid_items(opt(value));
        const auto view = valid_optional.as<TSD<i64, TS<i64>>>();

        static_assert(std::is_same_v<std::remove_cvref_t<decltype(optional_valid)>,
                                     wp<TSD<i64, TS<i64>>, optional, valid_items>>);
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(valid_optional)>,
                                     wp<TSD<i64, TS<i64>>, optional, valid_items>>);
        static_assert(std::is_same_v<std::remove_cvref_t<decltype(view)>,
                                     wp<TSD<i64, TS<i64>>, optional, valid_items>>);

        optional_input(optional_valid);
        optional_input(valid_optional);
    });
}

TEST_CASE("valid_items annotations require a capable receiver and survive references",
          "[feature:wiring][feature:operator][series:tsd][series:reference]") {
    const auto source = node([](Out<TSD<i64, TS<i64>>>) {});

    const auto reference_source = node([](Out<REF<TSD<i64, TS<i64>>>>) {});

    const auto capable = node([](In<TSD<i64, TS<i64>>, valid_items>) {});

    const auto ordinary = node([](In<TSD<i64, TS<i64>>>) {});

    testing::test_wiring([&] {
        capable(bg::valid_items(source()));
        capable(bg::valid_items(reference_source()));
        CHECK_THROWS_AS(ordinary(bg::valid_items(source())), TypeResolutionError);
    });
}

TEST_CASE("aggregate graph inputs preserve optionality",
          "[feature:wiring][validity:optional][series:value][shape:nested][definition:aggregate]") {
    struct optional_identity {
        wp<TS<i64>, optional> value;

        auto wire() const { return value; }
    };

    const auto identity = graph<optional_identity>();

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        const auto output = identity(opt(source()));
        CHECK(&output.type() == &TS<i64>::type());
    });
}

TEST_CASE("opt works with erased and named wiring arguments",
          "[feature:wiring][feature:operator][validity:optional][series:value]") {
    auto selected = std::string{};
    auto optional_only = operator_<auto(TS_T)->TS_T>{"optional_named", {"value"}};
    optional_only.overload(graph("name"_n = "optional", [&](wp<TS<i64>, optional> value) {
        selected = "optional";
        return value;
    }));

    const auto source = node([](Out<TS<i64>>) {});

    const auto required_input = node([](In<TS<i64>>) {});
    testing::test_wiring([&] {
        const auto value = source();
        const WiringPoint erased = value;

        (void)optional_only(opt(erased));
        CHECK(selected == "optional");

        (void)optional_only("value"_n = opt(erased));
        CHECK(selected == "optional");

        CHECK_THROWS_AS(required_input(opt(value)), TypeResolutionError);
    });
}

namespace {

template <template <typename, auto...> typename Field> struct optional_expansion_bundle {
    Field<TS<i64>> first;
    Field<TS<float>> second;
};

} // namespace

TEST_CASE("args and kwargs preserve opt on every expanded bundle field",
          "[feature:wiring][feature:variadic-wiring][validity:optional][series:tsb][schema:named]") {
    const auto source = node([](Out<TSB<optional_expansion_bundle>>) {});
    const auto optional_positional =
        graph("name"_n = "optional_positional", [](wp<TS<i64>, optional>, wp<TS<float>, optional>) {});

    const auto required_positional = graph("name"_n = "required_positional", [](wp<TS<i64>>, wp<TS<float>>) {});

    const auto optional_named = graph("name"_n = "optional_named",
                                      [](wp<TS<float>, "second"_n, optional>, wp<TS<i64>, "first"_n, optional>) {});
    const auto required_named =
        graph("name"_n = "required_named", [](wp<TS<float>, "second"_n>, wp<TS<i64>, "first"_n>) {});

    testing::test_wiring([&] {
        optional_positional(args(opt(source())));
        optional_named(kwargs(opt(source())));

        CHECK_THROWS_AS(required_positional(args(opt(source()))), TypeResolutionError);
        CHECK_THROWS_AS(required_named(kwargs(opt(source()))), TypeResolutionError);
    });
}

TEST_CASE("reference affinity precedes optional-input preference",
          "[feature:operator][feature:wiring][validity:optional][series:reference][series:value]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->void>{"reference_choice", {"value"}};
    operation.overload(graph("name"_n = "direct", [&](wp<TS<i64>>) { selected = "direct"; }));
    operation.overload(
        graph("name"_n = "reference", [&](wp<REF<TS<i64>>, optional>) { selected = "reference"; }));

    const auto reference = node([](Out<REF<TS<i64>>>) {});
    testing::test_wiring([&] { operation(reference()); });

    CHECK(selected == "reference");
}

TEST_CASE("implicit context optionality selects overloads and separates cache entries",
          "[feature:wiring][feature:operator][feature:context-time-series][validity:optional]"
          "[series:value]") {
    auto selected = std::string{};
    auto operation = operator_<auto()->TS_T>{"optional_context"};
    operation.overload(graph("name"_n = "required", [&](wp<TS<i64>, "value"_n, context> value) {
        selected = "required";
        return value;
    }));
    operation.overload(graph("name"_n = "optional",
                             [&](wp<TS<i64>, "value"_n, context, optional> value) {
                                 selected = "optional";
                                 return value;
                             }));

    const auto source = node([](Out<TS<i64>>) {});
    testing::test_wiring([&] {
        const auto value = source();

        with_context("value"_n = value, [&] { (void)operation(); });
        CHECK(selected == "required");

        with_context("value"_n = opt(value), [&] { (void)operation(); });
        CHECK(selected == "optional");

        with_context("value"_n = value, [&] { (void)operation(); });
        CHECK(selected == "required");
    });
}

TEST_CASE("variadic graph forwarding retains item optionality",
          "[feature:wiring][feature:operator][feature:variadic-wiring][validity:optional]"
          "[series:tsb][schema:generic][shape:doubly-nested]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"variadic_optional", {"values"}};
    operation.overload(graph("name"_n = "required", [&](wp<TSB<SCHEMA>, var_kwargs> values) {
        selected = "required";
        return values[0].time_series().as<TS<i64>>();
    }));
    operation.overload(graph("name"_n = "optional", [&](wp<TSB<SCHEMA>, var_kwargs, optional> values) {
        selected = "optional";
        return values[0].time_series().as<TS<i64>>();
    }));

    const auto forward = graph("name"_n = "forward", [&](wp<TSB<SCHEMA>, var_kwargs, optional> values) {
        return operation(values).as<TS<i64>>();
    });

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        (void)operation("value"_n = source());
        CHECK(selected == "required");

        (void)forward("value"_n = opt(source()));
        CHECK(selected == "optional");
    });
}

TEST_CASE("an opt-qualified direct named TSL selects an optional variadic overload",
          "[feature:wiring][feature:operator][feature:variadic-wiring][validity:optional]"
          "[series:fixed-tsl][shape:single-node]") {
    auto selected = std::string{};
    auto operation = operator_<auto(TS_T)->TS_T>{"optional_tsl", {"values"}};
    operation.overload(graph("name"_n = "required", [&](wp<TSL<TS_T, SIZE>, "values"_n, var_args> values) {
        selected = "required";
        return values[0].time_series().as<TS<i64>>();
    }));
    operation.overload(graph("name"_n = "optional", [&](wp<TSL<TS_T, SIZE>, "values"_n, var_args, optional> values) {
        selected = "optional";
        return values[0].time_series().as<TS<i64>>();
    }));

    const auto source = node([](Out<TS<i64>>) {});
    testing::test_wiring([&] {
        const auto collection = tsl(source(), source());

        (void)operation("values"_n = collection);
        CHECK(selected == "required");

        (void)operation("values"_n = opt(collection));
        CHECK(selected == "optional");
    });
}

TEST_CASE("variadic cache keys retain each member's optionality across a graph boundary",
          "[feature:wiring][feature:operator][feature:variadic-wiring][validity:optional]"
          "[series:tsb][schema:generic][shape:nested]") {
    auto requirement_calls = 0;
    auto operation = operator_<auto(TS_T)->void>{"member_optionalities", {"values"}};
    operation.overload(node(
        eval = [](In<TSB<SCHEMA>, var_kwargs, optional>) {},
        require = [&](const WiringArguments &arguments) {
            ++requirement_calls;
            CHECK(arguments.size() == 2);
            return accept;
        }));

    const auto forward = graph("name"_n = "forward", [&](wp<TSB<SCHEMA>, var_kwargs, optional> values) {
        operation(values);
    });

    const auto source = node([](Out<TS<i64>>) {});

    testing::test_wiring([&] {
        forward("first"_n = opt(source()), "second"_n = source());
        forward("first"_n = source(), "second"_n = opt(source()));
        forward("first"_n = opt(source()), "second"_n = source());
    });

    CHECK(requirement_calls == 2);
}

#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph.hpp>
#include <bg/node.hpp>
#include <bg/operator.hpp>
#include <bg/operator.hpp>
#include <bg/scalar/declaration.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/testing/test_graph.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/static_storage.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

TEST_CASE("reference and value consumers observe converted inputs on each tick",
          "[feature:wiring][feature:reference][feature:test-graph][series:reference]"
          "[binding:value-to-reference][transition:update]") {
    using namespace bg;

    const auto copy_reference = node([](In<REF<TS<i64>>> input, Out<REF<TS<i64>>> output) {
        output.copy(input);
    });

    const auto copy_value = node([](i64 value) { return value; });

    const auto through_reference = graph([&](wp<TS<i64>> value) {
        return copy_reference(value.as<REF<TS<i64>>>());
    });

    const auto through_value = graph([&](wp<TS<i64>> value) {
        return copy_value(value.as<REF<TS<i64>>>());
    });

    CHECK(testing::test_graph(through_reference, {3, 7, 11}).equals({3, 7, 11}));
    CHECK(testing::test_graph(through_value, {5, 9, 13}).equals({5, 9, 13}));
}

namespace {

template <template <typename, auto...> typename Field> struct OneValue {
    Field<bg::TS<bg::i64>> value;
};

template <template <typename, auto...> typename Field>
using CapturedSchema = bg::BundleSchemaVariableDeclaration<"CAPTURED", Field>;

class OverlayRecord final : public bg::details::WiringRecord {
  public:
    explicit OverlayRecord(const bg::TimeSeriesType &type) : WiringRecord{type} {}

  private:
    bg::WiringPoint materialize(const bg::TimeSeriesType &) const override { return point(*this); }
};

class CountingMaterialisationRecord final : public bg::details::WiringRecord {
  public:
    CountingMaterialisationRecord(const bg::TimeSeriesType &type, int &preparations, int &expansions,
                                  bg::WiringPoint result, bool throw_on_expansion,
                                  std::vector<bg::details::MaterialisationDependency> dependencies = {})
        : WiringRecord{type}, preparations_{preparations}, expansions_{expansions}, result_{result},
          throw_on_expansion_{throw_on_expansion}, dependencies_{std::move(dependencies)} {}

    bool has_default_materialisation() const noexcept override { return false; }
    bg::WiringPoint materialize(const bg::TimeSeriesType &) const override { return point(*this); }

    bg::details::MaterialisationPreparation
    prepare_materialisation(const bg::TimeSeriesType &) const override {
        ++preparations_;
        return {.dependencies = dependencies_};
    }

    bg::WiringPoint expand_materialisation(const bg::TimeSeriesType &, std::span<const bg::WiringArgument>,
                                           std::span<const bg::TimeSeriesType *const>) const override {
        ++expansions_;
        if (throw_on_expansion_) {
            throw std::runtime_error{"shared materialisation expansion failure"};
        }
        return result_;
    }

  private:
    int &preparations_;
    int &expansions_;
    bg::WiringPoint result_;
    bool throw_on_expansion_;
    std::vector<bg::details::MaterialisationDependency> dependencies_;
};

bool is_double_series(const bg::TimeSeriesType &type) noexcept { return &type == &bg::TS<double>::type(); }

} // namespace

TEST_CASE("deferred wiring points retain arguments and expand lazily once",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]"
          "[representation:free][shape:source]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto scalar_argument = bg::i64{41};
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input, scalar_argument);
    scalar_argument = 99;

    auto builder_calls = std::make_shared<int>();
    std::weak_ptr<int> builder_lifetime = builder_calls;
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [input_record = static_cast<bg::WiringPoint>(input).record(), builder_calls](
            bg::wp<bg::TS<bg::i64>> value, bg::i64 scalar) {
            CHECK(static_cast<bg::WiringPoint>(value).record() == input_record);
            CHECK(scalar == 41);
            ++*builder_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    builder_calls.reset();

    const auto options = deferred.materialisation_options();
    REQUIRE(options.size() == 1);
    CHECK(options[0].target_type == &bg::TSB<OneValue>::type());
    CHECK(options[0].cost == 0.0);
    REQUIRE(options[0].input_signature != nullptr);
    CHECK(options[0].input_signature->parameter_count() == 2);
    CHECK_FALSE(builder_lifetime.expired());
    const auto retained_builder_state = builder_lifetime.lock();
    REQUIRE(retained_builder_state);
    CHECK(*retained_builder_state == 0);

    const auto node_count = wiring.nodes().size();
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TSB<OneValue>::type()));
    const auto first = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());
    const auto second = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());
    CHECK(first.record() == second.record());
    CHECK(*retained_builder_state == 1);
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TSB<OneValue>::type()));
    CHECK(wiring.nodes().size() == node_count);

    const auto field = first.as<bg::TSB<OneValue>>()->value;
    CHECK(&field.type() == &bg::TS<bg::i64>::type());
    CHECK(static_cast<bg::WiringPoint>(field).record() == static_cast<bg::WiringPoint>(input).record());
}

TEST_CASE("default materialisation availability distinguishes lazy bundles from deferred points",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]"
          "[representation:free][shape:source]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bundle = bg::bundle<bg::TSB<OneValue>>("value"_n = input);
    const auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);

    CHECK(static_cast<bg::WiringPoint>(input).has_default_materialisation());
    CHECK(static_cast<bg::WiringPoint>(bundle).has_default_materialisation());
    CHECK_FALSE(static_cast<bg::WiringPoint>(bundle).record()->has_wired_output());
    CHECK_FALSE(static_cast<bg::WiringPoint>(deferred).has_default_materialisation());
    CHECK(wiring.materialisation_options(deferred).empty());
}

TEST_CASE("reference conversion selects advertised forms lazily and expands each callback once",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:reference]"
          "[binding:value-to-reference]") {
    using namespace bg;

    const auto integer_source = node([](Out<TS<i64>>) {});

    const auto float_source = node([](Out<TS<float>>) {});

    const auto consume = node([](In<REF<TS<float>>>) {});

    const auto consume_value = node([](In<TS<float>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    const auto input = integer_source();

    auto value_form = wiring.create_wp<TS<i64>>(input);
    auto value_builder_calls = 0;
    value_form.materialisation_option<TS<float>>(
        [&](wp<TS<i64>>) {
            ++value_builder_calls;
            return float_source();
        },
        0.0);
    const auto value_reference = value_form.as<REF<TS<float>>>();
    const auto repeated_value_reference = value_form.as<REF<TS<float>>>();

    auto reference_form = wiring.create_wp<TS<i64>>(input);
    auto reference_value_builder_calls = 0;
    auto reference_builder_calls = 0;
    reference_form.materialisation_option<TS<float>>(
        [&](wp<TS<i64>>) {
            ++reference_value_builder_calls;
            return float_source();
        },
        0.0);
    reference_form.materialisation_option<REF<TS<float>>>(
        [&](wp<TS<i64>>) {
            ++reference_builder_calls;
            return float_source();
        },
        1.0);
    const auto advertised_reference = reference_form.as<REF<TS<float>>>();
    const auto repeated_advertised_reference = reference_form.as<REF<TS<float>>>();

    CHECK(static_cast<WiringPoint>(value_reference).record() ==
          static_cast<WiringPoint>(repeated_value_reference).record());
    CHECK(static_cast<WiringPoint>(advertised_reference).record() ==
          static_cast<WiringPoint>(repeated_advertised_reference).record());
    CHECK(&advertised_reference.type() == &REF<TS<float>>::type());
    CHECK(value_builder_calls == 0);
    CHECK(reference_value_builder_calls == 0);
    CHECK(reference_builder_calls == 0);
    CHECK(wiring.nodes().empty());

    consume(value_reference);
    consume(advertised_reference);
    consume_value(advertised_reference);
    wiring.finalize();

    CHECK(value_builder_calls == 1);
    CHECK(reference_value_builder_calls == 0);
    CHECK(reference_builder_calls == 1);
}

TEST_CASE("reference conversion selects the cheapest compatible value form",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:reference]"
          "[binding:value-to-reference]") {
    using namespace bg;

    const auto source = node([](Out<TS<i64>>) {});

    const auto deduplicated_source = node([](Out<TS<float, TimeSeriesGuarantee::deduplicated>>) {});

    const auto final_source = node([](Out<TS<float, TimeSeriesGuarantee::final>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto deferred = wiring.create_wp<TS<i64>>(source());
    auto deduplicated_builder_calls = 0;
    auto final_builder_calls = 0;
    deferred.materialisation_option<TS<float, TimeSeriesGuarantee::deduplicated>>(
        [&](wp<TS<i64>>) {
            ++deduplicated_builder_calls;
            return deduplicated_source();
        },
        1.0);
    deferred.materialisation_option<TS<float, TimeSeriesGuarantee::final>>(
        [&](wp<TS<i64>>) {
            ++final_builder_calls;
            return final_source();
        },
        2.0);

    const auto reference = deferred.as<REF<TS<float>>>();

    CHECK(&reference.type() ==
          &REF<TS<float, TimeSeriesGuarantee::deduplicated>, TimeSeriesGuarantee::const_>::type());
    CHECK(deduplicated_builder_calls == 0);
    CHECK(final_builder_calls == 0);
    CHECK(wiring.nodes().empty());
}

TEST_CASE("reference conversion rejects equal-cost distinct value forms",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:reference]"
          "[binding:value-to-reference]") {
    using namespace bg;

    const auto source = node([](Out<TS<i64>>) {});

    const auto deduplicated_source = node([](Out<TS<float, TimeSeriesGuarantee::deduplicated>>) {});

    const auto final_source = node([](Out<TS<float, TimeSeriesGuarantee::final>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto deferred = wiring.create_wp<TS<i64>>(source());
    auto deduplicated_builder_calls = 0;
    auto final_builder_calls = 0;
    deferred.materialisation_option<TS<float, TimeSeriesGuarantee::deduplicated>>(
        [&](wp<TS<i64>>) {
            ++deduplicated_builder_calls;
            return deduplicated_source();
        },
        1.0);
    deferred.materialisation_option<TS<float, TimeSeriesGuarantee::final>>(
        [&](wp<TS<i64>>) {
            ++final_builder_calls;
            return final_source();
        },
        1.0);

    CHECK_THROWS_AS(deferred.as<REF<TS<float>>>(), WiringError);
    CHECK(deduplicated_builder_calls == 0);
    CHECK(final_builder_calls == 0);
    CHECK(wiring.nodes().empty());
}

TEST_CASE("reference conversion ignores incompatible advertised reference forms",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:reference]"
          "[guarantee:deduplicated]") {
    using namespace bg;

    using REFERENCE = REF<TS<float>, TimeSeriesGuarantee::ordinary>;
    using REQUESTED = REF<TS<float>, TimeSeriesGuarantee::deduplicated>;

    const auto source = node([](Out<TS<i64>>) {});

    const auto reference_source = node([](Out<REF<TS<float>, TimeSeriesGuarantee::ordinary>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto deferred = wiring.create_wp<TS<i64>>(source());
    deferred.materialisation_option<REFERENCE>(
        [reference_source](wp<TS<i64>>) { return reference_source(); }, 0.0);

    CHECK_THROWS_AS(deferred.as<REQUESTED>(), WiringError);
}

TEST_CASE("reference conversion resolves a generic target from the source type",
          "[feature:wiring][feature:reference][series:reference][declaration:generic]") {
    using namespace bg;

    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto reference = static_cast<WiringPoint>(source()).as<REF<TS_T>>();

    CHECK(&reference.type() == &REF<TS<i64>, TimeSeriesGuarantee::const_>::type());
    CHECK(wiring.nodes().empty());
}

TEST_CASE("reference conversion selects a lowest-cost reference form before checking ties",
          "[feature:wiring][feature:reference][series:reference][declaration:generic]") {
    using namespace bg;

    const auto source = node([](Out<TS<i64>>) {});

    const auto float_source = node([](Out<REF<TS<float>>>) {});

    const auto double_source = node([](Out<REF<TS<double>>>) {});

    const auto boolean_source = node([](Out<REF<TS<bool>>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto deferred = wiring.create_wp<TS<i64>>(source());
    deferred.materialisation_option<REF<TS<float>>>(
        [float_source](wp<TS<i64>>) { return float_source(); }, 2.0);
    deferred.materialisation_option<REF<TS<double>>>(
        [double_source](wp<TS<i64>>) { return double_source(); }, 2.0);
    deferred.materialisation_option<REF<TS<bool>>>(
        [boolean_source](wp<TS<i64>>) { return boolean_source(); }, 1.0);

    const auto reference = deferred.as<REF<TS_T>>();

    CHECK(&reference.type() == &REF<TS<bool>>::type());
}

TEST_CASE("reference conversion rejects equal-cost distinct advertised forms",
          "[feature:wiring][feature:reference][series:reference][declaration:generic]") {
    using namespace bg;

    const auto source = node([](Out<TS<i64>>) {});

    const auto float_source = node([](Out<REF<TS<float>>>) {});

    const auto double_source = node([](Out<REF<TS<double>>>) {});

    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto deferred = wiring.create_wp<TS<i64>>(source());
    deferred.materialisation_option<REF<TS<float>>>(
        [float_source](wp<TS<i64>>) { return float_source(); }, 1.0);
    deferred.materialisation_option<REF<TS<double>>>(
        [double_source](wp<TS<i64>>) { return double_source(); }, 1.0);

    CHECK_THROWS_AS(deferred.as<REF<TS_T>>(), WiringError);
}

TEST_CASE("deferred wiring points cache each advertised target independently",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]"
          "[representation:free][shape:fan-out]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto column_calls = 0;
    auto bundle_calls = 0;

    deferred.materialisation_option<bg::TS<bg::i64>>(
        [input, &column_calls](bg::wp<bg::TS<bg::i64>>) {
            ++column_calls;
            return static_cast<bg::WiringPoint>(input);
        },
        1.0);
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [&bundle_calls](bg::wp<bg::TS<bg::i64>> value) {
            ++bundle_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.0);

    wiring.require_materialisation(deferred, bg::TS<bg::i64>::type());
    wiring.require_materialisation(deferred, bg::TSB<OneValue>::type());
    CHECK(wiring.materialisation_required(deferred, bg::TS<bg::i64>::type()));
    CHECK(wiring.materialisation_required(deferred, bg::TSB<OneValue>::type()));
    CHECK_THROWS_AS(
        deferred.materialisation_option<bg::TSB<OneValue>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0),
        bg::WiringError);

    const auto as_column = wiring.expand_materialisation(deferred, bg::TS<bg::i64>::type());
    const auto as_bundle = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());
    const auto as_column_again = wiring.expand_materialisation(deferred, bg::TS<bg::i64>::type());
    const auto as_bundle_again = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());

    CHECK(as_column.record() == static_cast<bg::WiringPoint>(input).record());
    CHECK(as_bundle.record() == as_bundle_again.record());
    CHECK(as_column_again.record() == as_column.record());
    CHECK(column_calls == 1);
    CHECK(bundle_calls == 1);
    CHECK(wiring.materialisation_options(deferred).size() == 2);
}

TEST_CASE("deferred wiring validates options, freezes their set, and rejects implicit routes",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]"
          "[declaration:concrete]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<double>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto float_input = float_source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);

    CHECK_THROWS_AS(
        deferred.materialisation_option<bg::TS<bg::i64>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, -1.0),
        bg::WiringError);
    CHECK_THROWS_AS(
        deferred.materialisation_option<bg::TS<bg::i64>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; },
            std::numeric_limits<double>::infinity()),
        bg::WiringError);

    const auto empty_builder = [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; };
    auto input_signature = bg::details::make_graph_input_signature<decltype(empty_builder)>(
        bg::util::symbol{"materialisation"});
    CHECK_THROWS_AS(static_cast<bg::WiringPoint>(deferred).add_materialisation_option(
                        bg::TS<bg::i64>::type(), 0.0, input_signature, bg::WiringMaterialisationBuilder{}),
                    bg::WiringError);

    const auto output_builder = [input](bg::wp<bg::TS<bg::i64>>) { return input; };
    const auto output_signature = bg::details::make_graph_signature<decltype(output_builder)>(
        bg::util::symbol{"materialisation_with_output"});
    CHECK_THROWS_AS(static_cast<bg::WiringPoint>(deferred).add_materialisation_option(
                        bg::TS<bg::i64>::type(), 0.0, output_signature,
                        bg::WiringMaterialisationBuilder{[](std::span<const bg::WiringArgument>) {
                            return bg::WiringPoint{};
                        }}),
                    bg::WiringError);

    deferred.materialisation_option<bg::TSB<OneValue>>(
        [float_input](bg::wp<bg::TS<bg::i64>>) { return static_cast<bg::WiringPoint>(float_input); },
        0.0);
    CHECK_THROWS_AS(wiring.require_materialisation(deferred, bg::TS<bg::i64>::type()), bg::WiringError);
    wiring.require_materialisation(deferred, bg::TSB<OneValue>::type());

    auto no_options = wiring.create_wp<bg::TS<bg::i64>>(input);
    CHECK(wiring.materialisation_options(no_options).empty());
    CHECK_THROWS_AS(wiring.require_materialisation(no_options, bg::TS<bg::i64>::type()), bg::WiringError);
    CHECK_THROWS_AS(wiring.expand_materialisation(no_options, bg::TS<bg::i64>::type()), bg::WiringError);

    auto invalid_result = wiring.create_wp<bg::TS<bg::i64>>(input);
    invalid_result.materialisation_option<bg::TS<bg::i64>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    wiring.require_materialisation(invalid_result, bg::TS<bg::i64>::type());
    CHECK_THROWS_AS(wiring.expand_materialisation(invalid_result, bg::TS<bg::i64>::type()), bg::WiringError);

    auto frozen = wiring.create_wp<bg::TS<bg::i64>>(input);
    CHECK(wiring.materialisation_options(frozen).empty());
    CHECK_THROWS_AS(
        frozen.materialisation_option<bg::TS<bg::i64>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0),
        bg::WiringError);

    auto frozen_by_expansion = wiring.create_wp<bg::TS<bg::i64>>(input);
    frozen_by_expansion.materialisation_option<bg::TS<bg::i64>>(
        [input](bg::wp<bg::TS<bg::i64>>) { return static_cast<bg::WiringPoint>(input); }, 0.0);
    wiring.require_materialisation(frozen_by_expansion, bg::TS<bg::i64>::type());
    CHECK_THROWS_AS(
        frozen_by_expansion.materialisation_option<bg::TSB<OneValue>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 1.0),
        bg::WiringError);
}

TEST_CASE("deferred wiring rejects points from another graph wiring",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:source]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto first_wiring = bg::GraphWiring{};
    auto second_wiring = bg::GraphWiring{};

    bg::WiringPoint foreign_input;
    {
        const auto context = bg::WiringContext{second_wiring};
        foreign_input = source();
    }

    {
        const auto context = bg::WiringContext{first_wiring};
        CHECK_THROWS_AS(first_wiring.create_wp<bg::TS<bg::i64>>(foreign_input), bg::WiringError);
        const auto local_input = source();
        const auto deferred = first_wiring.create_wp<bg::TS<bg::i64>>(local_input);
        deferred.materialisation_option<bg::TS<bg::i64>>(
            [foreign_input](bg::wp<bg::TS<bg::i64>>) { return foreign_input; }, 0.0);
        first_wiring.require_materialisation(deferred, bg::TS<bg::i64>::type());
        CHECK_THROWS_AS(first_wiring.expand_materialisation(deferred, bg::TS<bg::i64>::type()), bg::WiringError);
    }
}

TEST_CASE("deferred wiring cannot retain temporary overlay records",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:source]") {
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto overlay = bg::details::WiringRecordOverlay{wiring};
    const auto temporary = overlay.add(std::make_unique<OverlayRecord>(bg::TS<bg::i64>::type()));
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>();

    CHECK_THROWS_AS(wiring.create_wp<bg::TS<bg::i64>>(temporary), bg::WiringError);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [temporary] { return temporary; }, 0.0);
    CHECK_NOTHROW(wiring.require_materialisation(deferred, bg::TS<bg::i64>::type()));
    CHECK_THROWS_AS(wiring.expand_materialisation(deferred, bg::TS<bg::i64>::type()), bg::WiringError);
}

TEST_CASE("deferred wiring requires resolved semantic and target types",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]"
          "[declaration:generic]") {
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto arguments = std::array<bg::WiringArgument, 0>{};

    CHECK_THROWS_AS(wiring.create_wp(bg::TS<bg::SCALAR>::type(), arguments), bg::WiringError);
    const auto deferred = wiring.create_wp<bg::TS<bg::i64>>();
    CHECK_THROWS_AS(
        deferred.materialisation_option<bg::TS<bg::SCALAR>>(
            [] { return bg::WiringPoint{}; }, 0.0),
        bg::WiringError);
}

TEST_CASE("signature matching enumerates materialisation options without running builders",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto builder_calls = 0;
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [&builder_calls](bg::wp<bg::TS<bg::i64>>) {
            ++builder_calls;
            return bg::WiringPoint{};
        },
        0.25);
    deferred.materialisation_option<bg::TSL<bg::TS<bg::i64>, 2>>(
        [&builder_calls](bg::wp<bg::TS<bg::i64>>) {
            ++builder_calls;
            return bg::WiringPoint{};
        },
        1.5);

    using AcceptedForm = bg::Union<"accepted_form", bg::TSB<OneValue>, bg::TSL<bg::TS<bg::i64>, 2>>;
    const auto signature = bg::Signature{bg::util::symbol{"consume"},
                                          {{bg::util::symbol{"value"}, &AcceptedForm::type()}}};
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto original_record = arguments[0].time_series().record();

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].argument_index == 0);
    CHECK(matches[0].argument_forms[0].offered_type == &bg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &bg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.25);
    CHECK(matches[0].materialisation_cost == 0.25);
    CHECK(matches[1].argument_forms[0].offered_type == &bg::TSL<bg::TS<bg::i64>, 2>::type());
    CHECK(matches[1].argument_forms[0].materialisation_cost == 1.5);
    CHECK(matches[1].materialisation_cost == 1.5);
    CHECK(builder_calls == 0);
    CHECK(arguments[0].time_series().record() == original_record);
    CHECK(&arguments[0].type() == &bg::TS<bg::i64>::type());
}

TEST_CASE("signature matching uses default materialisation and reports no unavailable route",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bundle = bg::bundle<bg::TSB<OneValue>>("value"_n = input);
    const auto bundle_argument = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(bundle)}};
    const auto bundle_signature = bg::Signature{
        bg::util::symbol{"consume_bundle"}, {{bg::util::symbol{"value"}, &bg::TSB<OneValue>::type()}}};

    const auto direct_matches = bundle_signature.match_wiring_arguments(bundle_argument);

    REQUIRE(direct_matches.size() == 1);
    CHECK(direct_matches[0].argument_forms[0].offered_type == &bundle_argument[0].type());
    CHECK(direct_matches[0].argument_forms[0].materialisation_target == nullptr);
    CHECK(direct_matches[0].materialisation_cost == 0.0);
    CHECK_FALSE(static_cast<bg::WiringPoint>(bundle).record()->has_wired_output());

    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [bundle](bg::wp<bg::TS<bg::i64>>) { return static_cast<bg::WiringPoint>(bundle); }, 0.0);
    const auto deferred_argument =
        std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto incompatible_signature = bg::Signature{
        bg::util::symbol{"consume_float"}, {{bg::util::symbol{"value"}, &bg::TS<double>::type()}}};

    CHECK(incompatible_signature.match_wiring_arguments(deferred_argument).empty());

    const auto generic_signature = bg::Signature{
        bg::util::symbol{"consume_any"}, {{bg::util::symbol{"value"}, &bg::TS_T::type()}}};
    const auto generic_matches = generic_signature.match_wiring_arguments(deferred_argument);
    REQUIRE(generic_matches.size() == 1);
    CHECK(generic_matches[0].argument_forms[0].offered_type == &bg::TSB<OneValue>::type());
    CHECK(&bg::TS_T::type().resolve(generic_matches[0].type_map) == &bg::TSB<OneValue>::type());
}

TEST_CASE("typed deferred bundles expose their materialisation options",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TSB<OneValue>>(input);
    auto builder_calls = 0;
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [&builder_calls](bg::wp<bg::TS<bg::i64>> value) {
            ++builder_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.75);

    const auto options = deferred.materialisation_options();
    const auto signature = bg::Signature{
        bg::util::symbol{"consume_bundle"}, {{bg::util::symbol{"value"}, &bg::TSB<OneValue>::type()}}};
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(options.size() == 1);
    CHECK(options[0].target_type == &bg::TSB<OneValue>::type());
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].materialisation_target == &bg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.75);
    CHECK(builder_calls == 0);
}

TEST_CASE("typed points accept advertised forms without selecting them",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"TYPED_POINT_FORM", bg::TSB<OneValue>, bg::TS<float>>;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(source());
    auto bundle_builder_calls = 0;
    auto float_builder_calls = 0;
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++bundle_builder_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    deferred.materialisation_option<bg::TS<float>>(
        [&](bg::wp<bg::TS<bg::i64>>) {
            ++float_builder_calls;
            return float_source();
        },
        1.0);
    const auto point = static_cast<bg::WiringPoint>(deferred);

    const auto bundle_point = bg::wp<bg::TSB<OneValue>>{point};
    const auto float_point = bg::wp<bg::TS<float>>{point};
    const auto union_point = bg::wp<FORM>{point};
    const auto reannotated_bundle_point = deferred.as<bg::TSB<OneValue>>();
    const auto reannotated_float_point = deferred.as<bg::TS<float>>();
    CHECK(static_cast<bg::WiringPoint>(bundle_point).record() == point.record());
    CHECK(static_cast<bg::WiringPoint>(float_point).record() == point.record());
    CHECK(static_cast<bg::WiringPoint>(union_point).record() == point.record());
    CHECK(static_cast<bg::WiringPoint>(reannotated_bundle_point).record() == point.record());
    CHECK(static_cast<bg::WiringPoint>(reannotated_float_point).record() == point.record());
    CHECK(&bundle_point.type() == &bg::TS<bg::i64>::type());
    CHECK(&float_point.type() == &bg::TS<bg::i64>::type());
    CHECK(&union_point.type() == &bg::TS<bg::i64>::type());
    CHECK(&reannotated_bundle_point.type() == &bg::TS<bg::i64>::type());
    CHECK(&reannotated_float_point.type() == &bg::TS<bg::i64>::type());
    CHECK(bundle_point.materialisation_options().size() == 2);
    CHECK(union_point.materialisation_options().size() == 2);
    CHECK_THROWS_AS(bg::wp<bg::TS<bool>>{point}, bg::WiringError);
    CHECK_FALSE(wiring.materialisation_required(point, bg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(point, bg::TS<float>::type()));
    CHECK(bundle_builder_calls == 0);
    CHECK(float_builder_calls == 0);

    const auto field = bundle_point->value;

    const auto consume = bg::node([](bg::In<bg::TS<bg::i64>>) {});
    consume(field);
    wiring.finalize();

    CHECK(wiring.materialisation_required(point, bg::TSB<OneValue>::type()));
    CHECK(bundle_builder_calls == 1);
    CHECK(float_builder_calls == 0);
}

TEST_CASE("direct graph calls select lazy forms without adding graph-boundary demands",
          "[feature:wiring-materialisation][feature:wiring][series:reference]") {
    using namespace bg::literals;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    auto builder_calls = 0;
    deferred.materialisation_option<bg::TS<float>>(
        [&](bg::wp<bg::TS<bg::i64>>) {
            ++builder_calls;
            return float_source();
        },
        0.0);

    const auto identity = bg::graph("name"_n = "identity", [](bg::wp<bg::TS<float>> value) { return value; });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(static_cast<bg::WiringPoint>(result).record() != static_cast<bg::WiringPoint>(deferred).record());
    CHECK(&result.type() == &bg::TS<float>::type());
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TS<float>::type()));
    CHECK(builder_calls == 0);

    const auto consume = bg::node([](bg::In<bg::TS<float>>) {});
    consume(result);
    wiring.finalize();

    CHECK(wiring.materialisation_required(deferred, bg::TS<float>::type()));
    CHECK(builder_calls == 1);
}

TEST_CASE("direct graph Union inputs keep the original point and its other forms",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"DIRECT_GRAPH_FORM", bg::TS<float>, bg::TS<double>>;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    const auto double_source = bg::node([](bg::Out<bg::TS<double>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    auto float_builder_calls = 0;
    auto double_builder_calls = 0;
    deferred.materialisation_option<bg::TS<float>>([float_source, &float_builder_calls](bg::wp<bg::TS<bg::i64>>) {
        ++float_builder_calls;
        return float_source();
    }, 0.0);
    deferred.materialisation_option<bg::TS<double>>([double_source, &double_builder_calls](bg::wp<bg::TS<bg::i64>>) {
        ++double_builder_calls;
        return double_source();
    }, 4.0);
    const auto original_record = static_cast<bg::WiringPoint>(deferred).record();
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    auto observed_options = std::size_t{};

    const auto identity = bg::graph("name"_n = "identity",
        [&](bg::wp<FORM> value) {
            observed_record = static_cast<bg::WiringPoint>(value).record();
            const auto options = value.materialisation_options();
            observed_options = options.size();
            REQUIRE(options.size() == 2);
            CHECK(options[0].target_type == &bg::TS<float>::type());
            CHECK(options[0].cost == 0.0);
            CHECK(options[1].target_type == &bg::TS<double>::type());
            CHECK(options[1].cost == 4.0);
            return bool_source();
        });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(observed_record == original_record);
    CHECK(observed_options == 2);
    CHECK(float_builder_calls == 0);
    CHECK(double_builder_calls == 0);
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TS<float>::type()));
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TS<double>::type()));
}

TEST_CASE("a graph callback that receives WiringArguments retains distinct equal-cost Union mappings",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"OBSERVED_GRAPH_FORM", bg::TS<float>, bg::TS<double>>;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    const auto double_source = bg::node([](bg::Out<bg::TS<double>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    deferred.materialisation_option<bg::TS<float>>(
        [float_source](bg::wp<bg::TS<bg::i64>>) { return float_source(); }, 0.0);
    deferred.materialisation_option<bg::TS<double>>(
        [double_source](bg::wp<bg::TS<bg::i64>>) { return double_source(); }, 0.0);

    auto observed_calls = 0;

    const auto observing = bg::graph("name"_n = "observing_union",
                                     [&](const bg::WiringArguments &, bg::wp<FORM>) {
                                         ++observed_calls;
                                         return bool_source();
                                     });
    CHECK_THROWS_AS(observing(deferred), bg::WiringError);
    CHECK(observed_calls == 0);

    const auto transparent = bg::graph("name"_n = "transparent_union",
                                       [&](bg::wp<FORM>) { return bool_source(); });
    CHECK_NOTHROW(transparent(deferred));
}

TEST_CASE("graph Union binding ignores mappings for unused alternatives",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"GRAPH_SCHEMA_FORM", bg::TS<bg::i64>, bg::TSB<bg::SCHEMA>>;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    deferred.materialisation_option<bg::TSB<OneValue>>([](bg::wp<bg::TS<bg::i64>> value) {
        return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
    }, 0.0);

    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);

    const auto identity = bg::graph("name"_n = "generic_union_identity", [&](bg::wp<FORM> value) {
        observed_record = static_cast<bg::WiringPoint>(value).record();
        return bool_source();
    });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(observed_record == static_cast<bg::WiringPoint>(deferred).record());
}

TEST_CASE("a direct graph Union input preserves its actual reference point",
          "[feature:wiring-materialisation][feature:wiring][feature:reference]"
          "[binding:reference-to-reference]") {
    using namespace bg::literals;
    using FORM = bg::Union<"DIRECT_GRAPH_REFERENCE_FORM", bg::REF<bg::TS<bg::i64>>, bg::TS<bg::i64>>;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto reference_source = bg::node([](bg::Out<bg::REF<bg::TS<bg::i64>>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    const auto reference = reference_source();
    const auto original_point = static_cast<bg::WiringPoint>(reference);
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    auto observed_type = static_cast<const bg::Type *>(nullptr);

    const auto identity = bg::graph("name"_n = "reference_identity", [&](bg::wp<FORM> input) {
        const auto point = static_cast<bg::WiringPoint>(input);
        observed_record = point.record();
        observed_type = &point.type();
        return bool_source();
    });

    const auto result = identity(reference);

    REQUIRE(result.has_output());
    CHECK(observed_record == original_point.record());
    CHECK(observed_type == &original_point.type());
}

TEST_CASE("direct graph calls defer selected reference forms until required",
          "[feature:wiring-materialisation][feature:wiring][series:reference]") {
    using namespace bg::literals;
    using REFERENCE = bg::REF<bg::TS<bg::i64>>;
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto reference_source = bg::node([](bg::Out<bg::REF<bg::TS<bg::i64>>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    auto value_builder_calls = 0;
    auto reference_builder_calls = 0;
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++value_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);
    deferred.materialisation_option<REFERENCE>(
        [&](bg::wp<bg::TS<bg::i64>>) {
            ++reference_builder_calls;
            return reference_source();
        },
        0.0);

    auto value_argument_type = static_cast<const bg::Type *>(nullptr);
    auto reference_argument_type = static_cast<const bg::Type *>(nullptr);

    const auto value_graph = bg::graph("name"_n = "value_form", [&](bg::wp<bg::TS<bg::i64>> value) {
        value_argument_type = &value.type();
        return bool_source();
    });

    const auto reference_graph = bg::graph("name"_n = "reference_form",
                                           [&](bg::wp<bg::REF<bg::TS<bg::i64>>> value) {
        reference_argument_type = &value.type();
        return bool_source();
    });

    const auto value_result = value_graph(deferred);
    REQUIRE(value_result.has_output());
    CHECK(value_argument_type == &bg::TS<bg::i64>::type());
    CHECK(value_builder_calls == 0);
    CHECK(reference_builder_calls == 0);

    const auto reference_result = reference_graph(deferred);
    REQUIRE(reference_result.has_output());
    CHECK(reference_argument_type == &REFERENCE::type());
    CHECK(value_builder_calls == 0);
    CHECK(reference_builder_calls == 0);
}

TEST_CASE("erased materialisation callbacks receive the selected concrete input form",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto upstream = wiring.create_wp<bg::TS<bg::i64>>(input);
    upstream.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>> value) { return bg::bundle<bg::TSB<OneValue>>("value"_n = value); }, 0.25);

    auto deferred = wiring.create_wp<bg::TS<bool>>(upstream);
    const auto input_signature = bg::Signature{
        bg::util::symbol{"materialisation"},
        {{bg::util::symbol{"value"}, &bg::TSB<OneValue>::type()}}};
    auto builder_calls = 0;
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    static_cast<bg::WiringPoint>(deferred).add_materialisation_option(
        bg::TSB<OneValue>::type(), 0.0, input_signature,
        bg::WiringMaterialisationBuilder{[&](std::span<const bg::WiringArgument> arguments) {
            ++builder_calls;
            REQUIRE(arguments.size() == 1);
            CHECK(bg::TSB<OneValue>::type().accepts(arguments[0].type()));
            CHECK(&arguments[0].type() == &arguments[0].time_series().type());
            observed_record = arguments[0].time_series().record();
            return arguments[0].time_series();
        }});

    const auto options = deferred.materialisation_options();
    REQUIRE(options.size() == 1);
    REQUIRE(options[0].input_signature != nullptr);
    CHECK(options[0].input_signature->parameter_count() == 1);
    CHECK(builder_calls == 0);

    const auto result = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());

    CHECK(builder_calls == 1);
    REQUIRE(observed_record != nullptr);
    CHECK(observed_record != static_cast<bg::WiringPoint>(upstream).record());
    CHECK(result.record() == observed_record);
    CHECK(result.record()->graph_owned());
}

TEST_CASE("typed materialisation callbacks receive the selected lazy bundle form",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto upstream = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto upstream_builder_calls = 0;
    upstream.materialisation_option<bg::TSB<OneValue>>(
        [&upstream_builder_calls](bg::wp<bg::TS<bg::i64>> value) {
            ++upstream_builder_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.25);

    const auto output = bg::node([](bg::Out<bg::TS<bool>>) {});
    const auto output_value = output();
    auto deferred = wiring.create_wp<bg::TS<bool>>(upstream);
    auto callback_calls = 0;
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    deferred.materialisation_option<bg::TS<bool>>(
        [&](bg::wp<bg::TSB<OneValue>> value) {
            ++callback_calls;
            observed_record = static_cast<bg::WiringPoint>(value).record();
            CHECK(bg::TSB<OneValue>::type().accepts(value.type()));
            CHECK(static_cast<bg::WiringPoint>(value).has_output());
            CHECK_FALSE(observed_record->has_wired_output());
            CHECK(observed_record->graph_owned());
            CHECK(bg::TS<bg::i64>::type().accepts(value->value.type()));
            return output_value;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, bg::TS<bool>::type());

    CHECK(callback_calls == 1);
    CHECK(upstream_builder_calls == 1);
    REQUIRE(observed_record != nullptr);
    CHECK(observed_record != static_cast<bg::WiringPoint>(upstream).record());
    CHECK(result.record() == static_cast<bg::WiringPoint>(output_value).record());
}

TEST_CASE("materialisation Union inputs retain the original point and its options",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"MATERIALISATION_FORM", bg::TS<bg::i64>, bg::TSB<OneValue>>;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    auto upstream = wiring.create_wp<bg::TS<bg::i64>>(input);
    upstream.materialisation_option<bg::TS<bg::i64>>(
        [](bg::wp<bg::TS<bg::i64>> value) { return static_cast<bg::WiringPoint>(value); }, 0.5);
    upstream.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>> value) { return bg::bundle<bg::TSB<OneValue>>("value"_n = value); }, 0.25);
    const auto upstream_record = static_cast<bg::WiringPoint>(upstream).record();

    auto deferred = wiring.create_wp<bg::TS<bool>>(upstream);
    auto builder_calls = 0;
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    auto observed_option_count = std::size_t{};
    deferred.materialisation_option<bg::TS<bool>>(
        [&](bg::wp<FORM> value) {
            ++builder_calls;
            observed_record = static_cast<bg::WiringPoint>(value).record();
            observed_option_count = value.materialisation_options().size();
            CHECK(&value.type() == &bg::TS<bg::i64>::type());
            return bool_input;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, bg::TS<bool>::type());

    CHECK(builder_calls == 1);
    CHECK(observed_record == upstream_record);
    CHECK(observed_option_count == 2);
    CHECK(result.record() == static_cast<bg::WiringPoint>(bool_input).record());
}

TEST_CASE("materialisation Union inputs keep their original semantic type without expanding options",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;
    using FORM = bg::Union<"MATERIALISATION_FORM", bg::TS<bg::i64>, bg::TSB<OneValue>>;

    const auto source = bg::node([](bg::Out<bg::TS<double>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    auto upstream = wiring.create_wp<bg::TS<double>>(input);
    auto upstream_builder_calls = 0;
    upstream.materialisation_option<bg::TS<bg::i64>>(
        [&upstream_builder_calls](bg::wp<bg::TS<double>>) {
            ++upstream_builder_calls;
            return bg::WiringPoint{};
        },
        0.5);
    upstream.materialisation_option<bg::TSB<OneValue>>(
        [&upstream_builder_calls](bg::wp<bg::TS<double>>) {
            ++upstream_builder_calls;
            return bg::WiringPoint{};
        },
        0.25);

    auto deferred = wiring.create_wp<bg::TS<bool>>(upstream);
    auto callback_calls = 0;
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);
    auto observed_option_count = std::size_t{};
    deferred.materialisation_option<bg::TS<bool>>(
        [&](bg::wp<FORM> value) {
            ++callback_calls;
            observed_record = static_cast<bg::WiringPoint>(value).record();
            observed_option_count = value.materialisation_options().size();
            CHECK(&value.type() == &bg::TS<double>::type());
            return bool_input;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, bg::TS<bool>::type());

    CHECK(callback_calls == 1);
    CHECK(upstream_builder_calls == 0);
    CHECK(observed_record == static_cast<bg::WiringPoint>(upstream).record());
    CHECK(observed_option_count == 2);
    CHECK(result.record() == static_cast<bg::WiringPoint>(bool_input).record());
}

TEST_CASE("variadic materialisation callback results retain graph-owned captured arguments",
          "[feature:wiring-materialisation][feature:wiring][feature:variadic-wiring][series:tsb]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();
    auto deferred = wiring.create_wp<bg::TSB<OneValue>>("value"_n = input);
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TSB<CapturedSchema>, bg::var_kwargs> values) {
            return static_cast<bg::WiringPoint>(values);
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());

    CHECK(result.record()->graph_owned());
    const auto materialized_bundle = wiring.complete_pending_wiring(result);
    const auto bundle_output = wiring.outputs()[wiring.output_index(materialized_bundle)];
    const auto &bundle_node = wiring.nodes()[bundle_output.node_index];
    REQUIRE(bundle_node.inputs().size() == 1);
    CHECK(bundle_node.inputs()[0].index() == wiring.output_index(input));

    const auto value = result.as<bg::TSB<OneValue>>()->value;
    CHECK(&value.graph() == &wiring);
    CHECK(static_cast<bg::WiringPoint>(value).record()->graph_owned());
    CHECK(bg::TS<bg::i64>::type().accepts(value.type()));
    CHECK(wiring.output_index(value) == wiring.output_index(input));
}

TEST_CASE("signature materialisation cost counts each point and target once",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto make_deferred = [&] {
        auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
        deferred.materialisation_option<bg::TSB<OneValue>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.25);
        deferred.materialisation_option<bg::TSL<bg::TS<bg::i64>, 2>>(
            [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 1.5);
        return deferred;
    };
    const auto first = make_deferred();
    const auto second = make_deferred();
    const auto same_target_signature = bg::Signature{
        bg::util::symbol{"same_target"},
        {{bg::util::symbol{"left"}, &bg::TSB<OneValue>::type()},
         {bg::util::symbol{"right"}, &bg::TSB<OneValue>::type()}}};
    const auto same_point_arguments = std::array{
        bg::WiringArgument{static_cast<bg::WiringPoint>(first)},
        bg::WiringArgument{static_cast<bg::WiringPoint>(first)}};
    const auto distinct_point_arguments = std::array{
        bg::WiringArgument{static_cast<bg::WiringPoint>(first)},
        bg::WiringArgument{static_cast<bg::WiringPoint>(second)}};

    const auto same_point_matches = same_target_signature.match_wiring_arguments(same_point_arguments);
    const auto distinct_point_matches = same_target_signature.match_wiring_arguments(distinct_point_arguments);

    REQUIRE(same_point_matches.size() == 1);
    REQUIRE(distinct_point_matches.size() == 1);
    CHECK(same_point_matches[0].materialisation_cost == 0.25);
    CHECK(distinct_point_matches[0].materialisation_cost == 0.5);

    const auto distinct_target_signature = bg::Signature{
        bg::util::symbol{"distinct_targets"},
        {{bg::util::symbol{"left"}, &bg::TSB<OneValue>::type()},
         {bg::util::symbol{"right"}, &bg::TSL<bg::TS<bg::i64>, 2>::type()}}};
    const auto distinct_target_matches = distinct_target_signature.match_wiring_arguments(same_point_arguments);

    REQUIRE(distinct_target_matches.size() == 1);
    CHECK(distinct_target_matches[0].materialisation_cost == 1.75);
}

TEST_CASE("binding cost charges one contained and one top-level reference adaptation per input",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:fixed-tsl]") {
    const auto source = bg::node([](bg::Out<bg::TSL<bg::TS<bg::i64>, 2>>) {});

    const auto scalar_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto scalar_input = scalar_source();
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(input)}};
    const auto scalar_arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(scalar_input)}};
    const auto not_preserved = std::array<unsigned char, 1>{0};

    auto contained_adaptation = bg::details::PreparedWiringCall{};
    contained_adaptation.resolution.signature.parameters.push_back(
        {bg::util::symbol{"values"}, &bg::TSL<bg::REF<bg::TS<bg::i64>>, 2>::type()});
    contained_adaptation.argument_forms.push_back(
        {0, &bg::TSL<bg::TS<bg::i64>, 2>::type(), nullptr, 0.0});
    contained_adaptation.preserved_inputs = not_preserved;

    CHECK(contained_adaptation.binding_cost(arguments) == 2.0);

    auto top_level_adaptation = bg::details::PreparedWiringCall{};
    top_level_adaptation.resolution.signature.parameters.push_back(
        {bg::util::symbol{"value"}, &bg::REF<bg::TS<bg::i64>>::type()});
    top_level_adaptation.argument_forms.push_back({0, &bg::TS<bg::i64>::type(), nullptr, 0.0});
    top_level_adaptation.preserved_inputs = not_preserved;

    CHECK(top_level_adaptation.binding_cost(scalar_arguments) == 1.0);
}

TEST_CASE("operator materialisation selects the cheapest concrete form and adapts it to the consumer type",
          "[feature:wiring-materialisation][feature:operator][series:reference]") {
    using namespace bg::literals;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(source());
    auto float_builder_calls = 0;
    auto bool_builder_calls = 0;
    deferred.materialisation_option<bg::TS<float>>(
        [&float_builder_calls, float_source](bg::wp<bg::TS<bg::i64>>) {
            ++float_builder_calls;
            return float_source();
        },
        0.75);
    deferred.materialisation_option<bg::TS<bool>>(
        [&bool_builder_calls, bool_source](bg::wp<bg::TS<bg::i64>>) {
            ++bool_builder_calls;
            return bool_source();
        },
        0.25);

    auto selected = std::string_view{};
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"select_form", {"value"}};
    operation.overload(bg::graph("name"_n = "float", [&](bg::wp<bg::TS<float>> value) {
        selected = "float";
        return value;
    }));
    operation.overload(bg::graph("name"_n = "bool", [&](bg::wp<bg::TS<bool>> value) {
        selected = "bool";
        return value;
    }));

    const auto result = operation(deferred);
    REQUIRE(result.has_output());
    CHECK(selected == "bool");
    CHECK(wiring.materialisation_required(deferred, bg::TS<bool>::type()));
    CHECK(wiring.materialisation_required(static_cast<bg::WiringPoint>(result), bg::TS<bool>::type()));
    CHECK_FALSE(wiring.materialisation_required(deferred, bg::TS<float>::type()));

    auto consume = bg::node([](bg::In<bg::REF<bg::TS<bool>>>) {});
    consume(result);
    wiring.finalize();

    CHECK(float_builder_calls == 0);
    CHECK(bool_builder_calls == 1);
    const auto &consumer = wiring.nodes().back();
    REQUIRE(consumer.inputs().size() == 1);
    const auto input = consumer.inputs()[0];
    CHECK_FALSE(input.is_inherited());
    CHECK(type_cast<bg::TimeSeriesReferenceType>(wiring.outputs()[input.index()].type) != nullptr);
}

TEST_CASE("operator cache hits rebind concrete demands to the current source",
          "[feature:wiring-materialisation][feature:operator][series:reference]") {
    class PassFloat final : public bg::WiringTemplate {
      public:
        explicit PassFloat(int &requirement_calls)
            : signature_{bg::util::symbol{"pass_float"},
                         {{bg::util::symbol{"value"}, &bg::TS<float>::type()}}, bg::TS<float>::type()},
              requirement_calls_{&requirement_calls} {}

        const bg::Signature &default_signature() const final { return signature_; }
        bg::RequirementResult requirement(const bg::WiringArguments &) const final {
            ++*requirement_calls_;
            return bg::accept;
        }
        std::optional<bg::WiringPoint> wire(bg::GraphWiring &, bg::ResolvedSignature,
                                            const bg::WiringArguments &,
                                            std::span<const bg::WiringArgument> arguments) const final {
            return arguments.front().time_series();
        }

      private:
        bg::Signature signature_;
        int *requirement_calls_;
    };

    auto requirement_calls = 0;
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"cached_form", {"value"}};
    operation.overload(PassFloat{requirement_calls});

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});
    const auto wire = [&](bg::GraphWiring &wiring) {
        const auto context = bg::WiringContext{wiring};
        const auto bind = [&] {
            auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
            deferred.materialisation_option<bg::TS<float>>(
                [float_source](bg::wp<bg::TS<bg::i64>>) { return float_source(); }, 0.0);
            const auto output = operation(deferred);
            REQUIRE(output.has_output());
            CHECK(wiring.materialisation_required(deferred, bg::TS<float>::type()));
        };
        bind();
        bind();
    };

    auto first_wiring = bg::GraphWiring{};
    wire(first_wiring);
    CHECK(requirement_calls == 1);
    first_wiring.finalize();

    auto second_wiring = bg::GraphWiring{};
    wire(second_wiring);
    CHECK(requirement_calls == 2);
    second_wiring.finalize();
}

TEST_CASE("rejected overload diagnostics distinguish offered forms from the joint binding",
          "[feature:wiring-materialisation][feature:operator][feature:wiring]") {
    class RejectFloat final : public bg::WiringTemplate {
      public:
        explicit RejectFloat(int &requirement_calls)
            : signature_{bg::util::symbol{"reject_float"},
                         {{bg::util::symbol{"value"}, &bg::TS<float>::type()}}, bg::TS<float>::type()},
              requirement_calls_{&requirement_calls} {}

        const bg::Signature &default_signature() const final { return signature_; }
        bg::RequirementResult requirement(const bg::WiringArguments &) const final {
            ++*requirement_calls_;
            return bg::reject("rejected after form matching");
        }
        std::optional<bg::WiringPoint> wire(bg::GraphWiring &, bg::ResolvedSignature,
                                            const bg::WiringArguments &,
                                            std::span<const bg::WiringArgument> arguments) const final {
            return arguments.front().time_series();
        }

      private:
        bg::Signature signature_;
        int *requirement_calls_;
    };

    using namespace bg::literals;
    auto requirement_calls = 0;
    auto operation = bg::operator_<auto(bg::TS_T)->bg::TS_T>{"form_diagnostic", {"value"}};
    operation.overload(RejectFloat{requirement_calls});

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    deferred.materialisation_option<bg::TS<float>>(
        [float_source](bg::wp<bg::TS<bg::i64>>) { return float_source(); }, 0.75);

    try {
        (void)operation(deferred);
        FAIL("expected rejected overload");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        REQUIRE(error.diagnostic()->arguments.size() == 1);
        REQUIRE(error.diagnostic()->arguments.front().offered_forms.size() == 1);
        CHECK(error.diagnostic()->arguments.front().offered_forms.front().type == "TS<float>");
        CHECK_FALSE(error.diagnostic()->arguments.front().offered_forms.front().direct);
        REQUIRE(error.diagnostic()->candidates.size() == 1);
        const auto &candidate = error.diagnostic()->candidates.front();
        REQUIRE(candidate.bindings.size() == 1);
        CHECK(candidate.bindings.front().offered_type == "TS<float>");
        CHECK(candidate.bindings.front().accepted_type == "TS<float>");
        CHECK(candidate.bindings.front().materialisation_type == "TS<float>");
        CHECK(candidate.bindings.front().cost == 0.75);
        REQUIRE(candidate.joint_materialisation_cost);
        CHECK(*candidate.joint_materialisation_cost == 0.75);
        CHECK(requirement_calls == 1);
    }
}

TEST_CASE("an item demand collects its selected parent form before expansion",
          "[feature:wiring-materialisation][feature:wiring][series:fixed-tsl][shape:source]") {
    using LIST = bg::TSL<bg::TS<bg::i64>, 1>;

    const auto source = bg::node([](bg::Out<LIST>) {});

    const auto consume = bg::node([](bg::In<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred_list = wiring.create_wp<LIST>(source());
    auto parent_demand_seen = false;
    deferred_list.materialisation_option<LIST>([&](bg::wp<LIST> value) {
        parent_demand_seen = wiring.materialisation_required(deferred_list, LIST::type());
        return value;
    }, 0.0);

    consume(deferred_list[0]);
    wiring.finalize();

    CHECK(parent_demand_seen);
}

TEST_CASE("an operator reports distinct output types from one overload as ambiguous",
          "[feature:wiring-materialisation][feature:operator][feature:type-system]") {
    using FORM = bg::Union<"AMBIGUOUS_INPUT_FORM", bg::TS<float>, bg::TS<bool>>;
    using RESULT = bg::TimeSeriesVariable<"AMBIGUOUS_RESULT">;

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(source());
    deferred.materialisation_option<bg::TS<float>>(
        [float_source](bg::wp<bg::TS<bg::i64>>) { return float_source(); }, 0.75);
    deferred.materialisation_option<bg::TS<bool>>(
        [bool_source](bg::wp<bg::TS<bg::i64>>) { return bool_source(); }, 0.25);

    auto operation = bg::operator_<auto(FORM)->RESULT>{"ambiguous_form", {"value"}};

    auto identity_function = [](bg::wp<FORM> value) -> bg::wp<RESULT> {
        return bg::wp<RESULT>::from_validated(static_cast<bg::WiringPoint>(value));
    };
    auto identity_resolver = bg::details::variable_resolver<RESULT>(
        [](const bg::WiringArguments &arguments) -> const bg::TimeSeriesType & {
            return static_cast<const bg::TimeSeriesType &>(arguments.resolved_type(0));
        });
    auto identity_signature = bg::details::make_graph_signature<decltype(identity_function)>(
        bg::util::symbol{"identity"}, std::span<const std::string_view>{},
        std::vector<bg::VariableResolver>{std::move(identity_resolver)});
    operation.overload(bg::GraphTemplate{std::move(identity_signature), std::move(identity_function)});

    try {
        (void)operation(deferred);
        FAIL("expected overload materialisation forms to resolve to different outputs");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("signature completes each form match with its original arguments and resolved types",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using FORM = bg::Union<"COMPLETED_FORM", bg::TSB<OneValue>, bg::TSL<bg::TS<bg::i64>, 2>>;
    using RESOLVED_SCALAR = bg::ScalarVariable<"FORM_RESOLVED_SCALAR">;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.25);
    deferred.materialisation_option<bg::TSL<bg::TS<bg::i64>, 2>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 1.5);
    const auto threshold = bg::i64{7};
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)},
                                      bg::WiringArgument{threshold}};
    const auto descriptors = std::array{bg::details::WiringArgumentDescriptor{arguments[0]},
                                        bg::details::WiringArgumentDescriptor{arguments[1]}};
    auto resolver_calls = 0;
    auto scalar_reads = 0;
    const auto record_scalar_read = [](void *context, std::size_t index, bg::Scalar<>) {
        if (index == 1) {
            ++*static_cast<int *>(context);
        }
    };
    const auto signature = bg::Signature{
        bg::util::symbol{"complete_form"},
        {{bg::util::symbol{"value"}, &FORM::type()},
         {bg::util::symbol{"threshold"}, &bg::int64_type()}},
        {bg::VariableResolver{
            &RESOLVED_SCALAR::type(),
            [&resolver_calls](const bg::TypeMap &, const bg::WiringArguments &resolved_arguments) -> bg::TypeMapValue {
                ++resolver_calls;
                const auto threshold_value = resolved_arguments.scalar(1);
                REQUIRE(threshold_value.has_value());
                CHECK(bg::scalar_cast<bg::i64>(*threshold_value).value() == 7);
                if (&resolved_arguments.resolved_type(0) == &bg::TSB<OneValue>::type()) {
                    return bg::TimeSeriesGuarantee::ordinary;
                }
                return static_cast<const bg::Type *>(&bg::int_type());
            }}}};
    auto matches = signature.match_wiring_arguments(arguments);
    auto context_type_map = bg::TypeMap{};
    const auto argument_context =
        bg::WiringArguments{signature, context_type_map, descriptors, &scalar_reads, record_scalar_read};
    auto failed_matches = 0;
    auto completed = std::vector<bg::ResolvedWiringSignatureMatch>{};

    REQUIRE(matches.size() == 2);
    for (auto &match : matches) {
        const auto expected_type = match.argument_forms[0].offered_type;
        try {
            completed.push_back(signature.complete_wiring_match(
                std::move(match), argument_context, [&](const bg::TypeMap &type_map) {
                    const auto resolved_arguments = argument_context.with_type_map(type_map);
                    CHECK(&resolved_arguments.original_type(0) == &bg::TS<bg::i64>::type());
                    CHECK(&resolved_arguments.resolved_type(0) == expected_type);
                    static_cast<void>(type_map.find(RESOLVED_SCALAR::type()));
                }));
        } catch (const bg::TypeResolutionError &) {
            ++failed_matches;
        }
    }

    CHECK(failed_matches == 1);
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].resolution.signature.parameters[0].type == &bg::TSL<bg::TS<bg::i64>, 2>::type());
    CHECK(completed[0].argument_forms[0].materialisation_target == &bg::TSL<bg::TS<bg::i64>, 2>::type());
    CHECK(completed[0].materialisation_cost == 1.5);
    CHECK(resolver_calls == 2);
    CHECK(scalar_reads == 2);
}

TEST_CASE("an open time series variable resolves from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [input](bg::wp<bg::TS<bg::i64>>) { return static_cast<bg::WiringPoint>(input); }, 4.0);
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{bg::util::symbol{"consume_any"},
                                          {{bg::util::symbol{"value"}, &bg::TS_T::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &bg::TS<bg::i64>::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &bg::TS<bg::i64>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 4.0);
    CHECK(&bg::TS_T::type().resolve(matches[0].type_map) == &bg::TS<bg::i64>::type());
    CHECK(matches[1].argument_forms[0].offered_type == &bg::TSB<OneValue>::type());
    CHECK(matches[1].argument_forms[0].materialisation_target == &bg::TSB<OneValue>::type());
    CHECK(matches[1].argument_forms[0].materialisation_cost == 0.0);
    CHECK(&bg::TS_T::type().resolve(matches[1].type_map) == &bg::TSB<OneValue>::type());

    auto conflicting_seed = bg::TypeMap{};
    const auto &variable = dynamic_cast<const bg::TypeVariableType<bg::TimeSeriesType> &>(bg::TS_T::type());
    conflicting_seed.set(variable, &bg::TS<double>::type());
    CHECK(signature.match_wiring_arguments(arguments, &conflicting_seed).empty());

    auto matching_seed = bg::TypeMap{};
    matching_seed.set(variable, &bg::TS<bg::i64>::type());
    CHECK(signature.match_wiring_arguments(arguments, &matching_seed).size() == 1);
}

TEST_CASE("independent form matches resolve variables nested in collection parameters",
          "[feature:wiring-materialisation][feature:wiring][series:fixed-tsl][schema:generic]") {
    using SOURCE = bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2,
                           bg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = bg::TSL<bg::TS<bg::i64>, 2>;
    using PARAMETER = bg::TSL<bg::TS_T, bg::SIZE>;

    const auto source = bg::node([](bg::Out<SOURCE>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](bg::wp<SOURCE> value) { return static_cast<bg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](bg::wp<SOURCE>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{bg::util::symbol{"consume_list"},
                                          {{bg::util::symbol{"values"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    CHECK(&bg::TS_T::type().resolve(matches[0].type_map) ==
          &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    CHECK(&bg::TS_T::type().resolve(matches[1].type_map) == &bg::TS<bg::i64>::type());
    for (const auto &match : matches) {
        CHECK(bg::details::size_variable_instance<bg::SIZE>().resolve(match.type_map) == 2);
    }
}

TEST_CASE("a failed direct-form match does not reject an offered bundle form",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using BUNDLE = bg::TSB<OneValue>;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<BUNDLE>([](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{bg::util::symbol{"consume_bundle"},
                                          {{bg::util::symbol{"value"}, &BUNDLE::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &BUNDLE::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &BUNDLE::type());
}

TEST_CASE("independent form matches resolve a shared variable from offered forms",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto int_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto double_source = bg::node([](bg::Out<bg::TS<double>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto int_input = int_source();
    const auto double_input = double_source();
    auto int_deferred = wiring.create_wp<bg::TS<bg::i64>>(int_input);
    int_deferred.materialisation_option<bg::TS<float>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    auto double_deferred = wiring.create_wp<bg::TS<double>>(double_input);
    double_deferred.materialisation_option<bg::TS<float>>(
        [](bg::wp<bg::TS<double>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(int_deferred)},
                                      bg::WiringArgument{static_cast<bg::WiringPoint>(double_deferred)}};
    const auto signature = bg::Signature{
        bg::util::symbol{"consume_shared"},
        {{bg::util::symbol{"left"}, &bg::TS_T::type()}, {bg::util::symbol{"right"}, &bg::TS_T::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);
    const auto reversed_arguments = std::array{arguments[1], arguments[0]};
    const auto reversed_matches = signature.match_wiring_arguments(reversed_arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &bg::TS<float>::type());
    CHECK(matches[0].argument_forms[1].offered_type == &bg::TS<float>::type());
    REQUIRE(reversed_matches.size() == 1);
    CHECK(reversed_matches[0].argument_forms[0].offered_type == &bg::TS<float>::type());
    CHECK(reversed_matches[0].argument_forms[1].offered_type == &bg::TS<float>::type());
}

TEST_CASE("reference target matching resolves each selected target structure",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    using SOURCE = bg::REF<bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>,
                            bg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = bg::REF<bg::TSL<bg::TS<double>, 2>>;
    using PARAMETER = bg::REF<bg::TS_T>;

    const auto source = bg::node([](bg::Out<SOURCE>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](bg::wp<SOURCE> value) { return static_cast<bg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](bg::wp<SOURCE>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{bg::util::symbol{"consume_reference"},
                                          {{bg::util::symbol{"value"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(&bg::TS_T::type().resolve(matches[0].type_map) ==
          &bg::TSL<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>, 2>::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    CHECK(&bg::TS_T::type().resolve(matches[1].type_map) == &bg::TSL<bg::TS<double>, 2>::type());
}

TEST_CASE("independent form matches preserve guarantees in captured bundle schemas",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:generic]") {
    using SOURCE = bg::TSB<OneValue, bg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = bg::TSB<OneValue>;
    using PARAMETER = bg::TSB<bg::SCHEMA>;

    const auto source = bg::node([](bg::Out<SOURCE>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](bg::wp<SOURCE> value) { return static_cast<bg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](bg::wp<SOURCE>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{bg::util::symbol{"consume_bundle"},
                                          {{bg::util::symbol{"value"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    const auto &schema_variable = bg::schema_type<bg::SCHEMA>::type();
    const auto *source_schema = dynamic_cast<const bg::Schema *>(&schema_variable.resolve(matches[0].type_map));
    const auto *alternative_schema = dynamic_cast<const bg::Schema *>(&schema_variable.resolve(matches[1].type_map));
    REQUIRE(source_schema != nullptr);
    REQUIRE(alternative_schema != nullptr);
    CHECK(source_schema->field(0).type == &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    CHECK(alternative_schema->field(0).type == &bg::TS<bg::i64>::type());
}

TEST_CASE("signature matching resolves constrained variables from selected forms",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 3.0);
    deferred.materialisation_option<bg::TS<double>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    static const auto constrained = bg::util::static_storage<bg::TypeVariableType<bg::TimeSeriesType>>{
        bg::util::symbol{"DOUBLE_SERIES"}, std::vector<const bg::TimeSeriesType *>{},
        std::vector<bg::TypeVariableType<bg::TimeSeriesType>::Constraint>{&is_double_series}};
    [[maybe_unused]] static const auto &constrained_registration =
        bg::TypeRegistry::register_static(constrained.get());
    const auto signature = bg::Signature{bg::util::symbol{"consume_double"},
                                          std::vector<bg::Parameter>{{bg::util::symbol{"value"},
                                                                      &constrained.get()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &bg::TS<double>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.0);
}

TEST_CASE("signature matching resolves a guarantee variable from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using GUARANTEED = bg::TS_T::with<bg::guarantee_variable<"MATCH_GUARANTEE">>;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 2.0);
    deferred.materialisation_option<bg::TS<double>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};

    const auto guaranteed_signature = bg::Signature{
        bg::util::symbol{"consume_guaranteed"}, {{bg::util::symbol{"value"}, &GUARANTEED::type()}}};

    const auto guaranteed_matches = guaranteed_signature.match_wiring_arguments(arguments);

    REQUIRE(guaranteed_matches.size() == 2);
    CHECK(guaranteed_matches[0].argument_forms[0].materialisation_target == &bg::TS<bg::i64>::type());
    CHECK(&GUARANTEED::type().resolve(guaranteed_matches[0].type_map) == &bg::TS<bg::i64>::type());
    CHECK(guaranteed_matches[1].argument_forms[0].materialisation_target == &bg::TS<double>::type());
    CHECK(&GUARANTEED::type().resolve(guaranteed_matches[1].type_map) == &bg::TS<double>::type());
}

TEST_CASE("signature matching resolves a reference target from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 2.0);
    deferred.materialisation_option<bg::TS<double>>(
        [](bg::wp<bg::TS<bg::i64>>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto reference_signature = bg::Signature{
        bg::util::symbol{"consume_reference"}, {{bg::util::symbol{"value"}, &bg::REF<bg::TS_T>::type()}}};

    const auto reference_matches = reference_signature.match_wiring_arguments(arguments);

    REQUIRE(reference_matches.size() == 2);
    CHECK(reference_matches[0].argument_forms[0].materialisation_target == &bg::TS<bg::i64>::type());
    CHECK(&bg::TS_T::type().resolve(reference_matches[0].type_map) == &bg::TS<bg::i64>::type());
    CHECK(reference_matches[1].argument_forms[0].materialisation_target == &bg::TS<double>::type());
    CHECK(&bg::TS_T::type().resolve(reference_matches[1].type_map) == &bg::TS<double>::type());
}

TEST_CASE("an ambiguous union form does not hide another valid materialisation form",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    using SOURCE = bg::REF<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>;
    using INPUT = bg::Union<"AMBIGUOUS_REFERENCE_FORM", bg::REF<bg::TS_T>, bg::TS_T>;

    const auto source = bg::node([](bg::Out<SOURCE>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred = wiring.create_wp<SOURCE>(source());
    deferred.materialisation_option<SOURCE>(
        [](bg::wp<SOURCE> value) { return static_cast<bg::WiringPoint>(value); }, 0.0);
    deferred.materialisation_option<bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>>(
        [](bg::wp<SOURCE>) { return bg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(deferred)}};
    const auto signature = bg::Signature{
        bg::util::symbol{"consume_union"}, {{bg::util::symbol{"value"}, &INPUT::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].materialisation_target ==
          &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
    CHECK(&bg::TS_T::type().resolve(matches[0].type_map) ==
          &bg::TS<bg::i64, bg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("signature matching branches independent unions and unifies shared inner variables",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto make_deferred = [&](double offset) {
        auto deferred = wiring.create_wp<bg::TS<bool>>(input);
        deferred.materialisation_option<bg::TS<bg::i64>>(
            [](bg::wp<bg::TS<bool>>) { return bg::WiringPoint{}; }, offset + 0.1);
        deferred.materialisation_option<bg::TS<double>>(
            [](bg::wp<bg::TS<bool>>) { return bg::WiringPoint{}; }, offset + 0.2);
        return deferred;
    };
    const auto left = make_deferred(0.0);
    const auto right = make_deferred(0.3);
    const auto arguments = std::array{bg::WiringArgument{static_cast<bg::WiringPoint>(left)},
                                      bg::WiringArgument{static_cast<bg::WiringPoint>(right)}};

    using LEFT_FORM = bg::Union<"left_form", bg::TS<bg::i64>, bg::TS<double>>;
    using RIGHT_FORM = bg::Union<"right_form", bg::TS<bg::i64>, bg::TS<double>>;
    const auto independent_signature = bg::Signature{
        bg::util::symbol{"independent"},
        {{bg::util::symbol{"left"}, &LEFT_FORM::type()}, {bg::util::symbol{"right"}, &RIGHT_FORM::type()}}};
    const auto independent_matches = independent_signature.match_wiring_arguments(arguments);

    REQUIRE(independent_matches.size() == 4);
    CHECK(LEFT_FORM::type().resolve(independent_matches[0].type_map).is_time_series());
    CHECK(RIGHT_FORM::type().resolve(independent_matches[0].type_map).is_time_series());
    CHECK(independent_matches[0].argument_forms[0].offered_type == &bg::TS<bg::i64>::type());
    CHECK(independent_matches[0].argument_forms[1].offered_type == &bg::TS<bg::i64>::type());
    CHECK(independent_matches[1].argument_forms[0].offered_type == &bg::TS<bg::i64>::type());
    CHECK(independent_matches[1].argument_forms[1].offered_type == &bg::TS<double>::type());

    using LEFT_COLUMN = bg::Union<"left_column", bg::TS<bg::SCALAR>>;
    using RIGHT_COLUMN = bg::Union<"right_column", bg::TS<bg::SCALAR>>;
    const auto shared_signature = bg::Signature{
        bg::util::symbol{"shared"},
        {{bg::util::symbol{"left"}, &LEFT_COLUMN::type()}, {bg::util::symbol{"right"}, &RIGHT_COLUMN::type()}}};
    const auto shared_matches = shared_signature.match_wiring_arguments(arguments);

    REQUIRE(shared_matches.size() == 2);
    for (const auto &match : shared_matches) {
        CHECK(match.argument_forms[0].offered_type == match.argument_forms[1].offered_type);
        CHECK(&LEFT_COLUMN::type().resolve(match.type_map) == match.argument_forms[0].offered_type);
        CHECK(&RIGHT_COLUMN::type().resolve(match.type_map) == match.argument_forms[1].offered_type);
    }
}

TEST_CASE("signature matching accepts the normalized argument of a variadic capture",
          "[feature:wiring-materialisation][feature:wiring][feature:variadic-wiring][series:tsb]"
          "[schema:generic]") {
    using namespace bg::literals;
    const auto capture =
        bg::graph("name"_n = "capture", [](bg::wp<bg::TSB<CapturedSchema>, bg::var_args>) {});

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto raw_arguments = std::array{bg::WiringArgument{source()}, bg::WiringArgument{source()}};
    auto overlay = bg::details::WiringRecordOverlay{wiring};

    const auto normalized = bg::details::probe_wiring_arguments(capture.signature(), raw_arguments, overlay);
    const auto matches = capture.signature().match_wiring_arguments(normalized.arguments);

    REQUIRE(normalized.arguments.size() == capture.signature().parameter_count());
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &normalized.arguments[0].type());
    CHECK(matches[0].argument_forms[0].materialisation_target == nullptr);
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.0);
}

TEST_CASE("materialisation collection retains context bindings for later expansion",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:fan-out]") {
    using namespace bg::literals;

    const auto first_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto second_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto consume = bg::node([](bg::In<bg::TSB<OneValue>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto first_provider = first_source();
    const auto second_provider = second_source();
    auto deferred = bg::WiringPoint{};
    {
        const auto scope = bg::with_context("value"_n = first_provider);
        deferred = wiring.create_wp<bg::TS<bg::i64>>();
    }
    auto observed_record = static_cast<const bg::details::WiringRecord *>(nullptr);

    auto input_signature = bg::Signature{
        bg::util::symbol{"materialisation"},
        {{.name = bg::util::symbol{"value"}, .type = &bg::TS<bg::i64>::type(), .context = true}}};
    {
        const auto scope = bg::with_context("value"_n = second_provider);
        deferred.add_materialisation_option(
            bg::TSB<OneValue>::type(), 0.0, std::move(input_signature),
            bg::WiringMaterialisationBuilder{[&](std::span<const bg::WiringArgument> arguments) {
                REQUIRE(arguments.size() == 1);
                observed_record = arguments[0].time_series().record();
                return bg::bundle<bg::TSB<OneValue>>("value"_n = arguments[0].time_series());
            }});
        consume(deferred);
    }
    {
        const auto scope = bg::with_context("value"_n = second_provider);
        wiring.finalize();
    }

    CHECK(observed_record == static_cast<bg::WiringPoint>(first_provider).record());
    CHECK(observed_record != static_cast<bg::WiringPoint>(second_provider).record());
}

TEST_CASE("failed transitive materialisation collection preserves committed requirements",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();

    auto previously_required = wiring.create_wp<bg::TS<bg::i64>>(input);
    previously_required.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>> value) { return bg::bundle<bg::TSB<OneValue>>("value"_n = value); }, 0.0);
    wiring.require_materialisation(previously_required, bg::TSB<OneValue>::type());

    auto deep = wiring.create_wp<bg::TS<bg::i64>>(input);
    deep.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TSL<bg::TS<bg::i64>, 2>>) { return bg::WiringPoint{}; }, 0.0);

    auto middle = wiring.create_wp<bg::TS<bg::i64>>(deep);
    middle.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TSB<OneValue>> value) { return value; }, 0.0);

    auto good = wiring.create_wp<bg::TS<bg::i64>>(input);
    good.materialisation_option<bg::TSB<OneValue>>(
        [](bg::wp<bg::TS<bg::i64>> value) { return bg::bundle<bg::TSB<OneValue>>("value"_n = value); }, 0.0);

    auto root = wiring.create_wp<bg::TS<bool>>(good, middle);
    root.materialisation_option<bg::TS<bool>>(
        [bool_input](bg::wp<bg::TSB<OneValue>>, bg::wp<bg::TSB<OneValue>>) { return bool_input; }, 0.0);

    CHECK(wiring.materialisation_required(previously_required, bg::TSB<OneValue>::type()));
    CHECK_THROWS_AS(wiring.require_materialisation(root, bg::TS<bool>::type()), bg::WiringError);

    CHECK(wiring.materialisation_required(previously_required, bg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(root, bg::TS<bool>::type()));
    CHECK_FALSE(wiring.materialisation_required(good, bg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(middle, bg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(deep, bg::TSB<OneValue>::type()));
}

TEST_CASE("materialisation builder failures include callback and wiring path context",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto consume = bg::node([](bg::In<bg::TS<float>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(source());
    deferred.materialisation_option<bg::TS<float>>([](bg::wp<bg::TS<bg::i64>>) -> bg::WiringPoint {
        throw std::runtime_error{"builder failure"};
    }, 0.0);
    consume(deferred);

    try {
        wiring.finalize();
        FAIL("expected materialisation callback failure");
    } catch (const bg::WiringError &error) {
        INFO(error.what());
        CHECK(std::string_view{error.what()}.find("builder failure") != std::string_view::npos);
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &context) {
            return context.stage == bg::WiringFailureStage::callback &&
                   context.operation == bg::util::symbol{"materialisation"} && !context.requested_type.empty();
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &context) {
            return context.stage == bg::WiringFailureStage::requirement && context.parameter.has_value() &&
                   !context.path.empty() && !context.requested_type.empty();
        }));
    }
}

TEST_CASE("materialisation builder failures preserve operator diagnostics",
          "[feature:wiring-materialisation][feature:wiring][feature:operator]") {
    using namespace bg::literals;

    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});

    const auto consume = bg::node([](bg::In<bg::TS<float>>) {});
    auto operation = bg::operator_<auto(bg::TS<bool>)->bg::TS<float>>{"builder_operator", {"value"}};
    operation.overload(bg::graph("name"_n = "boolean", [&](bg::wp<bg::TS<bool>>) { return float_source(); }));

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
    deferred.materialisation_option<bg::TS<float>>(
        [&](bg::wp<bg::TS<bg::i64>> value) { return operation(value); }, 0.0);
    consume(deferred);

    try {
        wiring.finalize();
        FAIL("expected overload-resolution failure inside materialisation builder");
    } catch (const bg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->operator_name == bg::util::symbol{"builder_operator"});
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == bg::WiringFailureStage::callback &&
                   entry.operation == bg::util::symbol{"materialisation"};
        }));
    }
}

TEST_CASE("materialisation errors retain causes and owned contexts after graph destruction",
          "[feature:wiring-materialisation][feature:wiring]") {
    using namespace bg::literals;

    auto saved_error = std::optional<bg::WiringError>{};
    {
        const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

        const auto consume = bg::node([](bg::In<bg::TS<float>>) {});
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        auto deferred = wiring.create_wp<bg::TS<bg::i64>>(source());
        deferred.materialisation_option<bg::TS<float>>(
            [](bg::wp<bg::TS<bg::i64>>) -> bg::WiringPoint { throw std::runtime_error{"owned cause"}; }, 0.0);
        consume(deferred);

        try {
            wiring.finalize();
            FAIL("expected materialisation failure");
        } catch (const bg::WiringError &error) {
            saved_error = error;
        }
    }

    REQUIRE(saved_error);
    REQUIRE(saved_error->cause());
    INFO(saved_error->what());
    CHECK(std::ranges::any_of(saved_error->contexts(), [](const auto &entry) {
        return !entry.path.empty() && !entry.requested_type.empty();
    }));
    try {
        std::rethrow_exception(saved_error->cause());
        FAIL("expected the retained original cause");
    } catch (const std::runtime_error &error) {
        CHECK(std::string_view{error.what()} == "owned cause");
    }
}

TEST_CASE("generated wiring result failures retain the selected call path",
          "[feature:wiring-materialisation][feature:wiring]") {
    using namespace bg::literals;

    const auto float_source = bg::node([](bg::Out<bg::TS<float>>) {});
    auto foreign_wiring = bg::GraphWiring{};
    const auto foreign_context = bg::WiringContext{foreign_wiring};
    const auto foreign_output = float_source();

    const auto invalid_graph = bg::graph("name"_n = "invalid_result", [foreign_output] { return foreign_output; });

    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    try {
        (void)invalid_graph();
        FAIL("expected a foreign wiring result");
    } catch (const bg::WiringError &error) {
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == bg::WiringFailureStage::callback && !entry.operation.text().empty() &&
                   entry.path.find("invalid_result") != std::string::npos;
        }));
    }
}

TEST_CASE("shared materialisation preparation retains each consumer context once",
          "[feature:wiring-materialisation][feature:wiring][shape:fan-out]") {
    using Dependency = bg::details::MaterialisationDependency;

    auto root_preparations = 0;
    auto left_preparations = 0;
    auto right_preparations = 0;
    auto shared_preparations = 0;
    auto root_expansions = 0;
    auto left_expansions = 0;
    auto right_expansions = 0;
    auto shared_expansions = 0;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto concrete = source();
    const auto make_record = [&](int &preparations, int &expansions, bool throws,
                                 std::vector<Dependency> dependencies = {}) {
        return wiring.register_record(std::make_unique<CountingMaterialisationRecord>(
            bg::TS<bg::i64>::type(), preparations, expansions, concrete, throws, std::move(dependencies)));
    };
    const auto shared = make_record(shared_preparations, shared_expansions, true);
    const auto left = make_record(left_preparations, left_expansions, false,
                                  {{shared, &bg::TS<bg::i64>::type(), bg::util::symbol{"left_to_shared"},
                                    "root[left]/shared", std::nullopt, std::nullopt}});
    const auto right = make_record(right_preparations, right_expansions, false,
                                   {{shared, &bg::TS<bg::i64>::type(), bg::util::symbol{"right_to_shared"},
                                     "root[right]/shared", std::nullopt, std::nullopt}});
    const auto root = make_record(root_preparations, root_expansions, false,
                                  {{left, &bg::TS<bg::i64>::type(), bg::util::symbol{"root_to_left"},
                                    "root/left", std::nullopt, std::nullopt},
                                   {right, &bg::TS<bg::i64>::type(), bg::util::symbol{"root_to_right"},
                                    "root/right", std::nullopt, std::nullopt}});
    wiring.require_materialisation(root, bg::TS<bg::i64>::type(),
                                  {.path = "consumer_a[read]", .consumer = bg::util::symbol{"consumer_a"}});
    wiring.require_materialisation(root, bg::TS<bg::i64>::type(),
                                  {.path = "consumer_b[read]", .consumer = bg::util::symbol{"consumer_b"}});

    try {
        wiring.finalize();
        FAIL("expected shared materialisation expansion failure");
    } catch (const bg::WiringError &error) {
        CHECK(root_preparations == 1);
        CHECK(left_preparations == 1);
        CHECK(right_preparations == 1);
        CHECK(shared_preparations == 1);
        CHECK(root_expansions == 1);
        CHECK(left_expansions == 1);
        CHECK(right_expansions == 0);
        CHECK(shared_expansions == 1);
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == bg::util::symbol{"consumer_a"} && entry.path == "consumer_a[read]";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == bg::util::symbol{"consumer_b"} && entry.path == "consumer_b[read]";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == bg::util::symbol{"left_to_shared"} &&
                   entry.path == "root[left]/shared";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == bg::util::symbol{"right_to_shared"} &&
                   entry.path == "root[right]/shared";
        }));
    }
}

TEST_CASE("materialisation return type failures report expected and actual types",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto integer_source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto boolean_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto producer_path = bg::WiringPath{nullptr, bg::util::symbol{"producer"}, bg::util::symbol{"create"}};
    auto deferred = [&] {
        const auto producer_context = bg::WiringContext{wiring, producer_path};
        auto point = wiring.create_wp<bg::TS<bg::i64>>(integer_source());
        point.materialisation_option<bg::TS<float>>(
            [boolean_source](bg::wp<bg::TS<bg::i64>>) { return boolean_source(); }, 0.0);
        return point;
    }();
    wiring.require_materialisation(deferred, bg::TS<float>::type(),
                                  {.path = "consumer[0]", .consumer = bg::util::symbol{"consumer"},
                                   .parameter = bg::util::symbol{"value"}});

    try {
        wiring.finalize();
        FAIL("expected incompatible materialisation result");
    } catch (const bg::WiringError &error) {
        auto expected = std::string{};
        auto actual = std::string{};
        bg::TS<float>::type().format_name(expected);
        bg::TS<bool>::type().format_name(actual);
        CHECK(std::ranges::any_of(error.contexts(), [&](const auto &entry) {
            return entry.expected_type == expected && entry.actual_type == actual;
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == bg::util::symbol{"materialisation"} &&
                   entry.path == "producer[create]";
        }));
    }
}

TEST_CASE("materialisation collection shares concrete demands with Union consumers",
          "[feature:wiring-materialisation][feature:wiring][schema:generic][shape:fan-out]") {
    using namespace bg::literals;
    using FORM = bg::Union<"SHARED_FORM", bg::TS<bg::i64>, bg::TSB<OneValue>>;

    const auto run = [&](bool concrete_first) {
        const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

        const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
        auto wiring = bg::GraphWiring{};
        const auto context = bg::WiringContext{wiring};
        const auto input = source();
        const auto bool_input = bool_source();

        auto shared = wiring.create_wp<bg::TS<bg::i64>>(input);
        auto concrete_builder_calls = 0;
        auto expression_builder_calls = 0;
        shared.materialisation_option<bg::TS<bg::i64>>(
            [&concrete_builder_calls](bg::wp<bg::TS<bg::i64>> value) {
                ++concrete_builder_calls;
                return static_cast<bg::WiringPoint>(value);
            },
            0.0);
        shared.materialisation_option<bg::TSB<OneValue>>(
            [&expression_builder_calls](bg::wp<bg::TS<bg::i64>> value) {
                ++expression_builder_calls;
                return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
            },
            0.0);

        auto union_consumer = wiring.create_wp<bg::TS<bool>>(shared);
        auto union_builder_calls = 0;
        auto union_saw_concrete_demand = false;
        union_consumer.materialisation_option<bg::TS<bool>>(
            [&](bg::wp<FORM>) {
                ++union_builder_calls;
                union_saw_concrete_demand = wiring.materialisation_required(shared, bg::TS<bg::i64>::type());
                return bool_input;
            },
            0.0);

        auto concrete_consumer = wiring.create_wp<bg::TS<bool>>(shared);
        auto concrete_consumer_calls = 0;
        concrete_consumer.materialisation_option<bg::TS<bool>>(
            [&concrete_consumer_calls, bool_input](bg::wp<bg::TS<bg::i64>>) {
                ++concrete_consumer_calls;
                return bool_input;
            },
            0.0);

        wiring.require_materialisation(union_consumer, bg::TS<bool>::type());
        wiring.require_materialisation(concrete_consumer, bg::TS<bool>::type());

        CHECK(wiring.materialisation_required(shared, bg::TS<bg::i64>::type()));
        CHECK_FALSE(wiring.materialisation_required(shared, bg::TSB<OneValue>::type()));
        CHECK(concrete_builder_calls == 0);
        CHECK(expression_builder_calls == 0);
        CHECK(union_builder_calls == 0);
        CHECK(concrete_consumer_calls == 0);

        const auto expand = [&](bg::WiringPoint point) {
            const auto result = wiring.expand_materialisation(point, bg::TS<bool>::type());
            CHECK(result.has_output());
        };
        if (concrete_first) {
            expand(concrete_consumer);
            expand(union_consumer);
        } else {
            expand(union_consumer);
            expand(concrete_consumer);
        }

        CHECK(union_builder_calls == 1);
        CHECK(union_saw_concrete_demand);
        CHECK(concrete_consumer_calls == 1);
        CHECK(concrete_builder_calls == 1);
        CHECK(expression_builder_calls == 0);
    };

    SECTION("Union expansion happens first") { run(false); }
    SECTION("concrete expansion happens first") { run(true); }
}


struct MaterialisationBundleSink {
    bg::In<bg::TSB<OneValue>> first;
    bg::In<bg::TSB<OneValue>> second;
    bg::Out<bg::TS<bg::i64>> output;

    void evaluate() const { output.set(*first->value + *second->value); }
};

TEST_CASE("normal materialization completes and caches a lazy callback result",
          "[feature:wiring-materialisation][feature:wiring][feature:collection-change][series:tsb]"
          "[schema:named][transition:update]") {
    using namespace bg::literals;

    auto callback_calls = 0;

    const auto sink = bg::node<MaterialisationBundleSink>();

    const auto tested = [&](bg::wp<bg::TS<bg::i64>> input) {
        auto deferred = bg::WiringContext::current().wiring().create_wp<bg::TSB<OneValue>>(input);
        deferred.materialisation_option<bg::TSB<OneValue>>(
            [&callback_calls](bg::wp<bg::TS<bg::i64>> value) {
                ++callback_calls;
                return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
            },
            0.0);
        return sink(deferred, deferred);
    };

    const auto actual = bg::testing::test_graph(tested, {7});

    CHECK(actual.equals({14}));
    CHECK(callback_calls == 1);
}

TEST_CASE("materialisation expansion permits another target on the same point",
          "[feature:wiring-materialisation][feature:wiring][schema:named][shape:chain]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto concrete_calls = 0;
    auto bundle_calls = 0;
    deferred.materialisation_option<bg::TSB<OneValue>>(
        [&bundle_calls](bg::wp<bg::TS<bg::i64>> value) {
            ++bundle_calls;
            return bg::bundle<bg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    deferred.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++concrete_calls;
            const auto alternate = wiring.expand_materialisation(deferred, bg::TSB<OneValue>::type());
            CHECK_FALSE(alternate.record()->has_wired_output());
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, bg::TS<bg::i64>::type());

    CHECK(result.record() == static_cast<bg::WiringPoint>(input).record());
    CHECK(concrete_calls == 1);
    CHECK(bundle_calls == 1);
}

TEST_CASE("materialisation dependency cycles fail and failed callbacks leave no active request",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:chain]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();

    auto expanding = wiring.create_wp<bg::TS<bg::i64>>();
    auto cycle = true;
    auto expansion_calls = 0;
    expanding.materialisation_option<bg::TS<bg::i64>>([&] {
        ++expansion_calls;
        if (cycle) {
            return wiring.expand_materialisation(expanding, bg::TS<bg::i64>::type());
        }
        return static_cast<bg::WiringPoint>(input);
    }, 0.0);

    CHECK_THROWS_AS(wiring.expand_materialisation(expanding, bg::TS<bg::i64>::type()), bg::WiringError);
    cycle = false;
    CHECK(wiring.expand_materialisation(expanding, bg::TS<bg::i64>::type()).record() ==
          static_cast<bg::WiringPoint>(input).record());
    CHECK(expansion_calls == 2);

    auto first = wiring.create_wp<bg::TS<bg::i64>>();
    auto second = wiring.create_wp<bg::TS<bg::i64>>();
    first.materialisation_option<bg::TS<bg::i64>>([second] { return static_cast<bg::WiringPoint>(second); }, 0.0);
    second.materialisation_option<bg::TS<bg::i64>>([first] { return static_cast<bg::WiringPoint>(first); }, 0.0);

    CHECK_THROWS_AS(wiring.materialize(first, bg::TS<bg::i64>::type()), bg::WiringError);

    auto retry = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto fail = true;
    auto retry_calls = 0;
    retry.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++retry_calls;
            if (fail) {
                throw bg::WiringError{"injected materialisation callback failure"};
            }
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);

    CHECK_THROWS_AS(wiring.materialize(retry, bg::TS<bg::i64>::type()), bg::WiringError);
    fail = false;
    CHECK(wiring.materialize(retry, bg::TS<bg::i64>::type()).record() ==
          static_cast<bg::WiringPoint>(input).record());
    CHECK(retry_calls == 2);
}

TEST_CASE("materialisation preparation follows concrete callback inputs through free bundles",
          "[feature:wiring-materialisation][feature:wiring][series:tsb]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});

    const auto bool_source = bg::node([](bg::Out<bg::TS<bool>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    (void)wiring.complete_pending_wiring();

    auto field = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto field_builder_calls = 0;
    field.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++field_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);
    const auto bundle = bg::bundle<bg::TSB<OneValue>>("value"_n = field);

    auto root = wiring.create_wp<bg::TS<bool>>(bundle);
    auto root_builder_calls = 0;
    root.materialisation_option<bg::TS<bool>>(
        [&](bg::wp<bg::TSB<OneValue>>) {
            ++root_builder_calls;
            return static_cast<bg::WiringPoint>(bool_input);
        },
        0.0);

    wiring.require_materialisation(root, bg::TS<bool>::type());

    CHECK(wiring.materialisation_required(root, bg::TS<bool>::type()));
    CHECK(wiring.materialisation_required(bundle, bg::TSB<OneValue>::type()));
    CHECK(wiring.materialisation_required(field, bg::TS<bg::i64>::type()));
    CHECK(field_builder_calls == 0);
    CHECK(root_builder_calls == 0);

    (void)wiring.expand_materialisation(root, bg::TS<bool>::type());
    CHECK(root_builder_calls == 1);
    CHECK(field_builder_calls == 0);

    (void)wiring.complete_pending_wiring(bundle);
    CHECK(field_builder_calls == 1);
}

TEST_CASE("materialisation preparation follows collection, item, and variadic dependencies",
          "[feature:wiring-materialisation][feature:wiring][series:fixed-tsl][series:tsb]") {
    using namespace bg::literals;

    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();

    auto field = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto field_builder_calls = 0;
    field.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++field_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);

    const auto list = bg::tsl(field);
    CHECK_FALSE(wiring.materialisation_required(field, bg::TS<bg::i64>::type()));
    wiring.require_materialisation(list, list.type());
    CHECK(wiring.materialisation_required(field, bg::TS<bg::i64>::type()));
    CHECK(field_builder_calls == 0);
    (void)wiring.complete_pending_wiring(list);
    CHECK(field_builder_calls == 1);

    auto list_source = bg::node([](bg::Out<bg::REF<bg::TSL<bg::TS<bg::i64>, 1>>>) {});
    const auto reference_list = list_source();
    const auto selected = bg::details::time_series_list_item(reference_list, 0);
    CHECK_FALSE(wiring.materialisation_required(selected, selected.type()));
    CHECK_FALSE(wiring.materialisation_required(reference_list, bg::TSL<bg::TS<bg::i64>, 1>::type()));
    wiring.require_materialisation(selected, selected.type());
    CHECK(wiring.materialisation_required(reference_list, bg::TSL<bg::TS<bg::i64>, 1>::type()));

    auto bundle_source = bg::node([](bg::Out<bg::TSB<OneValue>>) {});
    const auto source_bundle = bundle_source();
    const auto selected_bundle_field = source_bundle->value;
    CHECK_FALSE(wiring.materialisation_required(source_bundle, bg::TSB<OneValue>::type()));
    wiring.require_materialisation(selected_bundle_field, bg::TS<bg::i64>::type());
    CHECK(wiring.materialisation_required(source_bundle, bg::TSB<OneValue>::type()));

    auto variadic_field = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto variadic_builder_calls = 0;
    variadic_field.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++variadic_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);
    auto variadic_argument = bg::WiringArgument{variadic_field};
    variadic_argument.set_variadic_name(bg::util::symbol{"value"}, bg::details::VariadicMode::named);
    const auto *operations = bg::details::variadic_wiring_operations<bg::TSB<OneValue>>();
    REQUIRE(operations != nullptr);
    auto variadic = wiring.register_record(std::make_unique<bg::details::VariadicWiringRecord>(
        bg::TSB<OneValue>::type(), bg::details::VariadicMode::named, *operations,
        std::vector<bg::WiringArgument>{std::move(variadic_argument)}, nullptr));
    CHECK_FALSE(wiring.materialisation_required(variadic_field, bg::TS<bg::i64>::type()));
    wiring.require_materialisation(variadic, variadic.type());
    CHECK(wiring.materialisation_required(variadic_field, bg::TS<bg::i64>::type()));
    CHECK(variadic_builder_calls == 0);
    (void)wiring.complete_pending_wiring(variadic);
    CHECK(variadic_builder_calls == 1);

    auto variadic_list_field = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto variadic_list_builder_calls = 0;
    variadic_list_field.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++variadic_list_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);
    auto variadic_list_argument = bg::WiringArgument{variadic_list_field};
    const auto *list_operations =
        bg::details::variadic_wiring_operations<bg::TSL<bg::TS<bg::i64>, 1>>();
    REQUIRE(list_operations != nullptr);
    auto variadic_list = wiring.register_record(std::make_unique<bg::details::VariadicWiringRecord>(
        bg::TSL<bg::TS<bg::i64>, 1>::type(), bg::details::VariadicMode::positional, *list_operations,
        std::vector<bg::WiringArgument>{std::move(variadic_list_argument)}, nullptr));
    CHECK_FALSE(wiring.materialisation_required(variadic_list_field, bg::TS<bg::i64>::type()));
    wiring.require_materialisation(variadic_list, variadic_list.type());
    CHECK(wiring.materialisation_required(variadic_list_field, bg::TS<bg::i64>::type()));
    CHECK(variadic_list_builder_calls == 0);
    (void)wiring.complete_pending_wiring(variadic_list);
    CHECK(variadic_list_builder_calls == 1);

    auto direct_list_field = wiring.create_wp<bg::TS<bg::i64>>(input);
    auto direct_list_builder_calls = 0;
    direct_list_field.materialisation_option<bg::TS<bg::i64>>(
        [&](bg::wp<bg::TS<bg::i64>> value) {
            ++direct_list_builder_calls;
            return static_cast<bg::WiringPoint>(value);
        },
        0.0);
    const auto direct_list = bg::tsl(direct_list_field);
    auto direct_variadic = wiring.register_record(std::make_unique<bg::details::VariadicWiringRecord>(
        direct_list, bg::details::VariadicMode::positional, *list_operations));
    CHECK_FALSE(wiring.materialisation_required(direct_list_field, bg::TS<bg::i64>::type()));
    wiring.require_materialisation(direct_variadic, direct_variadic.type());
    CHECK(wiring.materialisation_required(direct_list_field, bg::TS<bg::i64>::type()));
    CHECK(direct_list_builder_calls == 0);
    (void)wiring.complete_pending_wiring(direct_variadic);
    CHECK(direct_list_builder_calls == 1);
}

TEST_CASE("materialisation preparation rejects an incompatible target for a default materialisation",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = bg::node([](bg::Out<bg::TS<bg::i64>>) {});
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(wiring.require_materialisation(input, bg::TS<bool>::type()), bg::WiringError);
}

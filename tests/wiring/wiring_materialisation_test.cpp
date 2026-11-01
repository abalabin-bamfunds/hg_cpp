#include <hg/graph/graph_wiring.hpp>
#include <hg/graph/details/wiring_materialisation.hpp>
#include <hg/graph/details/wiring_record.hpp>
#include <hg/graph/details/wiring_record_overlay.hpp>
#include <hg/graph/details/normalized_wiring_arguments.hpp>
#include <hg/graph/details/variadic_wiring_record.hpp>
#include <hg/graph/wiring_context.hpp>
#include <hg/graph.hpp>
#include <hg/node.hpp>
#include <hg/operator.hpp>
#include <hg/operator.hpp>
#include <hg/scalar/declaration.hpp>
#include <hg/scalar/scalar.hpp>
#include <hg/run_graph.hpp>
#include <hg/testing/test_wiring.hpp>
#include <hg/type/signature.hpp>
#include <hg/type/type_registry.hpp>
#include <hg/util/static_storage.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsl/tsl.hpp>

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
    using namespace hg;

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

    CHECK(hg::run_graph(through_reference, {3, 7, 11}).equals({3, 7, 11}));
    CHECK(hg::run_graph(through_value, {5, 9, 13}).equals({5, 9, 13}));
}

namespace {

template <template <typename, auto...> typename Field> struct OneValue {
    Field<hg::TS<hg::i64>> value;
};

template <template <typename, auto...> typename Field>
using CapturedSchema = hg::BundleSchemaVariableDeclaration<"CAPTURED", Field>;

class OverlayRecord final : public hg::details::WiringRecord {
  public:
    explicit OverlayRecord(const hg::TimeSeriesType &type) : WiringRecord{type} {}

  private:
    hg::WiringPoint materialize(const hg::TimeSeriesType &) const override { return point(*this); }
};

class CountingMaterialisationRecord final : public hg::details::WiringRecord {
  public:
    CountingMaterialisationRecord(const hg::TimeSeriesType &type, int &preparations, int &expansions,
                                  hg::WiringPoint result, bool throw_on_expansion,
                                  std::vector<hg::details::MaterialisationDependency> dependencies = {})
        : WiringRecord{type}, preparations_{preparations}, expansions_{expansions}, result_{result},
          throw_on_expansion_{throw_on_expansion}, dependencies_{std::move(dependencies)} {}

    bool has_default_materialisation() const noexcept override { return false; }
    hg::WiringPoint materialize(const hg::TimeSeriesType &) const override { return point(*this); }

    hg::details::MaterialisationPreparation
    prepare_materialisation(const hg::TimeSeriesType &) const override {
        ++preparations_;
        return {.dependencies = dependencies_};
    }

    hg::WiringPoint expand_materialisation(const hg::TimeSeriesType &, std::span<const hg::WiringArgument>,
                                           std::span<const hg::TimeSeriesType *const>) const override {
        ++expansions_;
        if (throw_on_expansion_) {
            throw std::runtime_error{"shared materialisation expansion failure"};
        }
        return result_;
    }

  private:
    int &preparations_;
    int &expansions_;
    hg::WiringPoint result_;
    bool throw_on_expansion_;
    std::vector<hg::details::MaterialisationDependency> dependencies_;
};

bool is_double_series(const hg::TimeSeriesType &type) noexcept { return &type == &hg::TS<double>::type(); }

} // namespace

TEST_CASE("deferred wiring points retain arguments and expand lazily once",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]"
          "[representation:free][shape:source]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto scalar_argument = hg::i64{41};
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input, scalar_argument);
    scalar_argument = 99;

    auto builder_calls = std::make_shared<int>();
    std::weak_ptr<int> builder_lifetime = builder_calls;
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [input_record = static_cast<hg::WiringPoint>(input).record(), builder_calls](
            hg::wp<hg::TS<hg::i64>> value, hg::i64 scalar) {
            CHECK(static_cast<hg::WiringPoint>(value).record() == input_record);
            CHECK(scalar == 41);
            ++*builder_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    builder_calls.reset();

    const auto options = deferred.materialisation_options();
    REQUIRE(options.size() == 1);
    CHECK(options[0].target_type == &hg::TSB<OneValue>::type());
    CHECK(options[0].cost == 0.0);
    REQUIRE(options[0].input_signature != nullptr);
    CHECK(options[0].input_signature->parameter_count() == 2);
    CHECK_FALSE(builder_lifetime.expired());
    const auto retained_builder_state = builder_lifetime.lock();
    REQUIRE(retained_builder_state);
    CHECK(*retained_builder_state == 0);

    const auto node_count = wiring.nodes().size();
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TSB<OneValue>::type()));
    const auto first = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());
    const auto second = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());
    CHECK(first.record() == second.record());
    CHECK(*retained_builder_state == 1);
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TSB<OneValue>::type()));
    CHECK(wiring.nodes().size() == node_count);

    const auto field = first.as<hg::TSB<OneValue>>()->value;
    CHECK(&field.type() == &hg::TS<hg::i64>::type());
    CHECK(static_cast<hg::WiringPoint>(field).record() == static_cast<hg::WiringPoint>(input).record());
}

TEST_CASE("default materialisation availability distinguishes lazy bundles from deferred points",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]"
          "[representation:free][shape:source]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bundle = hg::bundle<hg::TSB<OneValue>>("value"_n = input);
    const auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);

    CHECK(static_cast<hg::WiringPoint>(input).has_default_materialisation());
    CHECK(static_cast<hg::WiringPoint>(bundle).has_default_materialisation());
    CHECK_FALSE(static_cast<hg::WiringPoint>(bundle).record()->has_wired_output());
    CHECK_FALSE(static_cast<hg::WiringPoint>(deferred).has_default_materialisation());
    CHECK(wiring.materialisation_options(deferred).empty());
}

TEST_CASE("reference conversion selects advertised forms lazily and expands each callback once",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:reference]"
          "[binding:value-to-reference]") {
    using namespace hg;

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
    using namespace hg;

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
    using namespace hg;

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
    using namespace hg;

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
    using namespace hg;

    const auto source = node([](Out<TS<i64>>) {});
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};

    const auto reference = static_cast<WiringPoint>(source()).as<REF<TS_T>>();

    CHECK(&reference.type() == &REF<TS<i64>, TimeSeriesGuarantee::const_>::type());
    CHECK(wiring.nodes().empty());
}

TEST_CASE("reference conversion selects a lowest-cost reference form before checking ties",
          "[feature:wiring][feature:reference][series:reference][declaration:generic]") {
    using namespace hg;

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
    using namespace hg;

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
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto column_calls = 0;
    auto bundle_calls = 0;

    deferred.materialisation_option<hg::TS<hg::i64>>(
        [input, &column_calls](hg::wp<hg::TS<hg::i64>>) {
            ++column_calls;
            return static_cast<hg::WiringPoint>(input);
        },
        1.0);
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [&bundle_calls](hg::wp<hg::TS<hg::i64>> value) {
            ++bundle_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.0);

    wiring.require_materialisation(deferred, hg::TS<hg::i64>::type());
    wiring.require_materialisation(deferred, hg::TSB<OneValue>::type());
    CHECK(wiring.materialisation_required(deferred, hg::TS<hg::i64>::type()));
    CHECK(wiring.materialisation_required(deferred, hg::TSB<OneValue>::type()));
    CHECK_THROWS_AS(
        deferred.materialisation_option<hg::TSB<OneValue>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0),
        hg::WiringError);

    const auto as_column = wiring.expand_materialisation(deferred, hg::TS<hg::i64>::type());
    const auto as_bundle = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());
    const auto as_column_again = wiring.expand_materialisation(deferred, hg::TS<hg::i64>::type());
    const auto as_bundle_again = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());

    CHECK(as_column.record() == static_cast<hg::WiringPoint>(input).record());
    CHECK(as_bundle.record() == as_bundle_again.record());
    CHECK(as_column_again.record() == as_column.record());
    CHECK(column_calls == 1);
    CHECK(bundle_calls == 1);
    CHECK(wiring.materialisation_options(deferred).size() == 2);
}

TEST_CASE("deferred wiring validates options, freezes their set, and rejects implicit routes",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]"
          "[declaration:concrete]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<double>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto float_input = float_source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);

    CHECK_THROWS_AS(
        deferred.materialisation_option<hg::TS<hg::i64>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, -1.0),
        hg::WiringError);
    CHECK_THROWS_AS(
        deferred.materialisation_option<hg::TS<hg::i64>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; },
            std::numeric_limits<double>::infinity()),
        hg::WiringError);

    const auto empty_builder = [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; };
    auto input_signature = hg::details::make_graph_input_signature<decltype(empty_builder)>(
        hg::util::symbol{"materialisation"});
    CHECK_THROWS_AS(static_cast<hg::WiringPoint>(deferred).add_materialisation_option(
                        hg::TS<hg::i64>::type(), 0.0, input_signature, hg::WiringMaterialisationBuilder{}),
                    hg::WiringError);

    const auto output_builder = [input](hg::wp<hg::TS<hg::i64>>) { return input; };
    const auto output_signature = hg::details::make_graph_signature<decltype(output_builder)>(
        hg::util::symbol{"materialisation_with_output"});
    CHECK_THROWS_AS(static_cast<hg::WiringPoint>(deferred).add_materialisation_option(
                        hg::TS<hg::i64>::type(), 0.0, output_signature,
                        hg::WiringMaterialisationBuilder{[](std::span<const hg::WiringArgument>) {
                            return hg::WiringPoint{};
                        }}),
                    hg::WiringError);

    deferred.materialisation_option<hg::TSB<OneValue>>(
        [float_input](hg::wp<hg::TS<hg::i64>>) { return static_cast<hg::WiringPoint>(float_input); },
        0.0);
    CHECK_THROWS_AS(wiring.require_materialisation(deferred, hg::TS<hg::i64>::type()), hg::WiringError);
    wiring.require_materialisation(deferred, hg::TSB<OneValue>::type());

    auto no_options = wiring.create_wp<hg::TS<hg::i64>>(input);
    CHECK(wiring.materialisation_options(no_options).empty());
    CHECK_THROWS_AS(wiring.require_materialisation(no_options, hg::TS<hg::i64>::type()), hg::WiringError);
    CHECK_THROWS_AS(wiring.expand_materialisation(no_options, hg::TS<hg::i64>::type()), hg::WiringError);

    auto invalid_result = wiring.create_wp<hg::TS<hg::i64>>(input);
    invalid_result.materialisation_option<hg::TS<hg::i64>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    wiring.require_materialisation(invalid_result, hg::TS<hg::i64>::type());
    CHECK_THROWS_AS(wiring.expand_materialisation(invalid_result, hg::TS<hg::i64>::type()), hg::WiringError);

    auto frozen = wiring.create_wp<hg::TS<hg::i64>>(input);
    CHECK(wiring.materialisation_options(frozen).empty());
    CHECK_THROWS_AS(
        frozen.materialisation_option<hg::TS<hg::i64>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0),
        hg::WiringError);

    auto frozen_by_expansion = wiring.create_wp<hg::TS<hg::i64>>(input);
    frozen_by_expansion.materialisation_option<hg::TS<hg::i64>>(
        [input](hg::wp<hg::TS<hg::i64>>) { return static_cast<hg::WiringPoint>(input); }, 0.0);
    wiring.require_materialisation(frozen_by_expansion, hg::TS<hg::i64>::type());
    CHECK_THROWS_AS(
        frozen_by_expansion.materialisation_option<hg::TSB<OneValue>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 1.0),
        hg::WiringError);
}

TEST_CASE("deferred wiring rejects points from another graph wiring",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:source]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto first_wiring = hg::GraphWiring{};
    auto second_wiring = hg::GraphWiring{};

    hg::WiringPoint foreign_input;
    {
        const auto context = hg::WiringContext{second_wiring};
        foreign_input = source();
    }

    {
        const auto context = hg::WiringContext{first_wiring};
        CHECK_THROWS_AS(first_wiring.create_wp<hg::TS<hg::i64>>(foreign_input), hg::WiringError);
        const auto local_input = source();
        const auto deferred = first_wiring.create_wp<hg::TS<hg::i64>>(local_input);
        deferred.materialisation_option<hg::TS<hg::i64>>(
            [foreign_input](hg::wp<hg::TS<hg::i64>>) { return foreign_input; }, 0.0);
        first_wiring.require_materialisation(deferred, hg::TS<hg::i64>::type());
        CHECK_THROWS_AS(first_wiring.expand_materialisation(deferred, hg::TS<hg::i64>::type()), hg::WiringError);
    }
}

TEST_CASE("deferred wiring cannot retain temporary overlay records",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:source]") {
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto overlay = hg::details::WiringRecordOverlay{wiring};
    const auto temporary = overlay.add(std::make_unique<OverlayRecord>(hg::TS<hg::i64>::type()));
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>();

    CHECK_THROWS_AS(wiring.create_wp<hg::TS<hg::i64>>(temporary), hg::WiringError);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [temporary] { return temporary; }, 0.0);
    CHECK_NOTHROW(wiring.require_materialisation(deferred, hg::TS<hg::i64>::type()));
    CHECK_THROWS_AS(wiring.expand_materialisation(deferred, hg::TS<hg::i64>::type()), hg::WiringError);
}

TEST_CASE("deferred wiring requires resolved semantic and target types",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]"
          "[declaration:generic]") {
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto arguments = std::array<hg::WiringArgument, 0>{};

    CHECK_THROWS_AS(wiring.create_wp(hg::TS<hg::SCALAR>::type(), arguments), hg::WiringError);
    const auto deferred = wiring.create_wp<hg::TS<hg::i64>>();
    CHECK_THROWS_AS(
        deferred.materialisation_option<hg::TS<hg::SCALAR>>(
            [] { return hg::WiringPoint{}; }, 0.0),
        hg::WiringError);
}

TEST_CASE("signature matching enumerates materialisation options without running builders",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto builder_calls = 0;
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [&builder_calls](hg::wp<hg::TS<hg::i64>>) {
            ++builder_calls;
            return hg::WiringPoint{};
        },
        0.25);
    deferred.materialisation_option<hg::TSL<hg::TS<hg::i64>, 2>>(
        [&builder_calls](hg::wp<hg::TS<hg::i64>>) {
            ++builder_calls;
            return hg::WiringPoint{};
        },
        1.5);

    using AcceptedForm = hg::Union<"accepted_form", hg::TSB<OneValue>, hg::TSL<hg::TS<hg::i64>, 2>>;
    const auto signature = hg::Signature{hg::util::symbol{"consume"},
                                          {{hg::util::symbol{"value"}, &AcceptedForm::type()}}};
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto original_record = arguments[0].time_series().record();

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].argument_index == 0);
    CHECK(matches[0].argument_forms[0].offered_type == &hg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &hg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.25);
    CHECK(matches[0].materialisation_cost == 0.25);
    CHECK(matches[1].argument_forms[0].offered_type == &hg::TSL<hg::TS<hg::i64>, 2>::type());
    CHECK(matches[1].argument_forms[0].materialisation_cost == 1.5);
    CHECK(matches[1].materialisation_cost == 1.5);
    CHECK(builder_calls == 0);
    CHECK(arguments[0].time_series().record() == original_record);
    CHECK(&arguments[0].type() == &hg::TS<hg::i64>::type());
}

TEST_CASE("signature matching uses default materialisation and reports no unavailable route",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bundle = hg::bundle<hg::TSB<OneValue>>("value"_n = input);
    const auto bundle_argument = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(bundle)}};
    const auto bundle_signature = hg::Signature{
        hg::util::symbol{"consume_bundle"}, {{hg::util::symbol{"value"}, &hg::TSB<OneValue>::type()}}};

    const auto direct_matches = bundle_signature.match_wiring_arguments(bundle_argument);

    REQUIRE(direct_matches.size() == 1);
    CHECK(direct_matches[0].argument_forms[0].offered_type == &bundle_argument[0].type());
    CHECK(direct_matches[0].argument_forms[0].materialisation_target == nullptr);
    CHECK(direct_matches[0].materialisation_cost == 0.0);
    CHECK_FALSE(static_cast<hg::WiringPoint>(bundle).record()->has_wired_output());

    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [bundle](hg::wp<hg::TS<hg::i64>>) { return static_cast<hg::WiringPoint>(bundle); }, 0.0);
    const auto deferred_argument =
        std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto incompatible_signature = hg::Signature{
        hg::util::symbol{"consume_float"}, {{hg::util::symbol{"value"}, &hg::TS<double>::type()}}};

    CHECK(incompatible_signature.match_wiring_arguments(deferred_argument).empty());

    const auto generic_signature = hg::Signature{
        hg::util::symbol{"consume_any"}, {{hg::util::symbol{"value"}, &hg::TS_T::type()}}};
    const auto generic_matches = generic_signature.match_wiring_arguments(deferred_argument);
    REQUIRE(generic_matches.size() == 1);
    CHECK(generic_matches[0].argument_forms[0].offered_type == &hg::TSB<OneValue>::type());
    CHECK(&hg::TS_T::type().resolve(generic_matches[0].type_map) == &hg::TSB<OneValue>::type());
}

TEST_CASE("deferred materialisation selects the cheapest concrete callback input",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg;
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto input = wiring.create_wp<TS<i64>>();
    input.materialisation_option<TS<float>>([] { return WiringPoint{}; }, 2.0);
    input.materialisation_option<TS<double>>([] { return WiringPoint{}; }, 2.0);
    input.materialisation_option<TS<i64>>([] { return WiringPoint{}; }, 1.0);

    auto deferred = wiring.create_wp<TS<i64>>(input);
    deferred.materialisation_option<TS<bool>>([](wp<TS_T>) { return WiringPoint{}; }, 0.0);

    wiring.require_materialisation(deferred, TS<bool>::type());

    CHECK_FALSE(wiring.materialisation_required(input, TS<float>::type()));
    CHECK_FALSE(wiring.materialisation_required(input, TS<double>::type()));
    CHECK(wiring.materialisation_required(input, TS<i64>::type()));
}

TEST_CASE("deferred materialisation rejects equal-cost concrete callback inputs",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg;
    auto wiring = GraphWiring{};
    const auto context = WiringContext{wiring};
    auto input = wiring.create_wp<TS<i64>>();
    input.materialisation_option<TS<float>>([] { return WiringPoint{}; }, 1.0);
    input.materialisation_option<TS<double>>([] { return WiringPoint{}; }, 1.0);

    auto deferred = wiring.create_wp<TS<i64>>(input);
    deferred.materialisation_option<TS<bool>>([](wp<TS_T>) { return WiringPoint{}; }, 0.0);

    CHECK_THROWS_AS(wiring.require_materialisation(deferred, TS<bool>::type()), WiringError);
    CHECK_FALSE(wiring.materialisation_required(input, TS<float>::type()));
    CHECK_FALSE(wiring.materialisation_required(input, TS<double>::type()));
}

TEST_CASE("typed deferred bundles expose their materialisation options",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TSB<OneValue>>(input);
    auto builder_calls = 0;
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [&builder_calls](hg::wp<hg::TS<hg::i64>> value) {
            ++builder_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.75);

    const auto options = deferred.materialisation_options();
    const auto signature = hg::Signature{
        hg::util::symbol{"consume_bundle"}, {{hg::util::symbol{"value"}, &hg::TSB<OneValue>::type()}}};
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(options.size() == 1);
    CHECK(options[0].target_type == &hg::TSB<OneValue>::type());
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].materialisation_target == &hg::TSB<OneValue>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.75);
    CHECK(builder_calls == 0);
}

TEST_CASE("typed points accept advertised forms without selecting them",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"TYPED_POINT_FORM", hg::TSB<OneValue>, hg::TS<float>>;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(source());
    auto bundle_builder_calls = 0;
    auto float_builder_calls = 0;
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++bundle_builder_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    deferred.materialisation_option<hg::TS<float>>(
        [&](hg::wp<hg::TS<hg::i64>>) {
            ++float_builder_calls;
            return float_source();
        },
        1.0);
    const auto point = static_cast<hg::WiringPoint>(deferred);

    const auto bundle_point = hg::wp<hg::TSB<OneValue>>{point};
    const auto float_point = hg::wp<hg::TS<float>>{point};
    const auto union_point = hg::wp<FORM>{point};
    const auto reannotated_bundle_point = deferred.as<hg::TSB<OneValue>>();
    const auto reannotated_float_point = deferred.as<hg::TS<float>>();
    CHECK(static_cast<hg::WiringPoint>(bundle_point).record() == point.record());
    CHECK(static_cast<hg::WiringPoint>(float_point).record() == point.record());
    CHECK(static_cast<hg::WiringPoint>(union_point).record() == point.record());
    CHECK(static_cast<hg::WiringPoint>(reannotated_bundle_point).record() == point.record());
    CHECK(static_cast<hg::WiringPoint>(reannotated_float_point).record() == point.record());
    CHECK(&bundle_point.type() == &hg::TS<hg::i64>::type());
    CHECK(&float_point.type() == &hg::TS<hg::i64>::type());
    CHECK(&union_point.type() == &hg::TS<hg::i64>::type());
    CHECK(&reannotated_bundle_point.type() == &hg::TS<hg::i64>::type());
    CHECK(&reannotated_float_point.type() == &hg::TS<hg::i64>::type());
    CHECK(bundle_point.materialisation_options().size() == 2);
    CHECK(union_point.materialisation_options().size() == 2);
    CHECK_THROWS_AS(hg::wp<hg::TS<bool>>{point}, hg::WiringError);
    CHECK_FALSE(wiring.materialisation_required(point, hg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(point, hg::TS<float>::type()));
    CHECK(bundle_builder_calls == 0);
    CHECK(float_builder_calls == 0);

    const auto field = bundle_point->value;

    const auto consume = hg::node([](hg::In<hg::TS<hg::i64>>) {});
    consume(field);
    wiring.finalize();

    CHECK(wiring.materialisation_required(point, hg::TSB<OneValue>::type()));
    CHECK(bundle_builder_calls == 1);
    CHECK(float_builder_calls == 0);
}

TEST_CASE("direct graph calls select lazy forms without adding graph-boundary demands",
          "[feature:wiring-materialisation][feature:wiring][series:reference]") {
    using namespace hg::literals;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    auto builder_calls = 0;
    deferred.materialisation_option<hg::TS<float>>(
        [&](hg::wp<hg::TS<hg::i64>>) {
            ++builder_calls;
            return float_source();
        },
        0.0);

    const auto identity = hg::graph("name"_n = "identity", [](hg::wp<hg::TS<float>> value) { return value; });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(static_cast<hg::WiringPoint>(result).record() != static_cast<hg::WiringPoint>(deferred).record());
    CHECK(&result.type() == &hg::TS<float>::type());
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TS<float>::type()));
    CHECK(builder_calls == 0);

    const auto consume = hg::node([](hg::In<hg::TS<float>>) {});
    consume(result);
    wiring.finalize();

    CHECK(wiring.materialisation_required(deferred, hg::TS<float>::type()));
    CHECK(builder_calls == 1);
}

TEST_CASE("direct graph Union inputs keep the original point and its other forms",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"DIRECT_GRAPH_FORM", hg::TS<float>, hg::TS<double>>;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    const auto double_source = hg::node([](hg::Out<hg::TS<double>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    auto float_builder_calls = 0;
    auto double_builder_calls = 0;
    deferred.materialisation_option<hg::TS<float>>([float_source, &float_builder_calls](hg::wp<hg::TS<hg::i64>>) {
        ++float_builder_calls;
        return float_source();
    }, 0.0);
    deferred.materialisation_option<hg::TS<double>>([double_source, &double_builder_calls](hg::wp<hg::TS<hg::i64>>) {
        ++double_builder_calls;
        return double_source();
    }, 4.0);
    const auto original_record = static_cast<hg::WiringPoint>(deferred).record();
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    auto observed_options = std::size_t{};

    const auto identity = hg::graph("name"_n = "identity",
        [&](hg::wp<FORM> value) {
            observed_record = static_cast<hg::WiringPoint>(value).record();
            const auto options = value.materialisation_options();
            observed_options = options.size();
            REQUIRE(options.size() == 2);
            CHECK(options[0].target_type == &hg::TS<float>::type());
            CHECK(options[0].cost == 0.0);
            CHECK(options[1].target_type == &hg::TS<double>::type());
            CHECK(options[1].cost == 4.0);
            return bool_source();
        });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(observed_record == original_record);
    CHECK(observed_options == 2);
    CHECK(float_builder_calls == 0);
    CHECK(double_builder_calls == 0);
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TS<float>::type()));
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TS<double>::type()));
}

TEST_CASE("a graph callback that receives WiringArguments retains distinct equal-cost Union mappings",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"OBSERVED_GRAPH_FORM", hg::TS<float>, hg::TS<double>>;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    const auto double_source = hg::node([](hg::Out<hg::TS<double>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    deferred.materialisation_option<hg::TS<float>>(
        [float_source](hg::wp<hg::TS<hg::i64>>) { return float_source(); }, 0.0);
    deferred.materialisation_option<hg::TS<double>>(
        [double_source](hg::wp<hg::TS<hg::i64>>) { return double_source(); }, 0.0);

    auto observed_calls = 0;

    const auto observing = hg::graph("name"_n = "observing_union",
                                     [&](const hg::WiringArguments &, hg::wp<FORM>) {
                                         ++observed_calls;
                                         return bool_source();
                                     });
    CHECK_THROWS_AS(observing(deferred), hg::WiringError);
    CHECK(observed_calls == 0);

    const auto transparent = hg::graph("name"_n = "transparent_union",
                                       [&](hg::wp<FORM>) { return bool_source(); });
    CHECK_NOTHROW(transparent(deferred));
}

TEST_CASE("graph Union binding ignores mappings for unused alternatives",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"GRAPH_SCHEMA_FORM", hg::TS<hg::i64>, hg::TSB<hg::SCHEMA>>;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    deferred.materialisation_option<hg::TSB<OneValue>>([](hg::wp<hg::TS<hg::i64>> value) {
        return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
    }, 0.0);

    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);

    const auto identity = hg::graph("name"_n = "generic_union_identity", [&](hg::wp<FORM> value) {
        observed_record = static_cast<hg::WiringPoint>(value).record();
        return bool_source();
    });

    const auto result = identity(deferred);

    REQUIRE(result.has_output());
    CHECK(observed_record == static_cast<hg::WiringPoint>(deferred).record());
}

TEST_CASE("a direct graph Union input preserves its actual reference point",
          "[feature:wiring-materialisation][feature:wiring][feature:reference]"
          "[binding:reference-to-reference]") {
    using namespace hg::literals;
    using FORM = hg::Union<"DIRECT_GRAPH_REFERENCE_FORM", hg::REF<hg::TS<hg::i64>>, hg::TS<hg::i64>>;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto reference_source = hg::node([](hg::Out<hg::REF<hg::TS<hg::i64>>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    const auto reference = reference_source();
    const auto original_point = static_cast<hg::WiringPoint>(reference);
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    auto observed_type = static_cast<const hg::Type *>(nullptr);

    const auto identity = hg::graph("name"_n = "reference_identity", [&](hg::wp<FORM> input) {
        const auto point = static_cast<hg::WiringPoint>(input);
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
    using namespace hg::literals;
    using REFERENCE = hg::REF<hg::TS<hg::i64>>;
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto reference_source = hg::node([](hg::Out<hg::REF<hg::TS<hg::i64>>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    auto value_builder_calls = 0;
    auto reference_builder_calls = 0;
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++value_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);
    deferred.materialisation_option<REFERENCE>(
        [&](hg::wp<hg::TS<hg::i64>>) {
            ++reference_builder_calls;
            return reference_source();
        },
        0.0);

    auto value_argument_type = static_cast<const hg::Type *>(nullptr);
    auto reference_argument_type = static_cast<const hg::Type *>(nullptr);

    const auto value_graph = hg::graph("name"_n = "value_form", [&](hg::wp<hg::TS<hg::i64>> value) {
        value_argument_type = &value.type();
        return bool_source();
    });

    const auto reference_graph = hg::graph("name"_n = "reference_form",
                                           [&](hg::wp<hg::REF<hg::TS<hg::i64>>> value) {
        reference_argument_type = &value.type();
        return bool_source();
    });

    const auto value_result = value_graph(deferred);
    REQUIRE(value_result.has_output());
    CHECK(value_argument_type == &hg::TS<hg::i64>::type());
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
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto upstream = wiring.create_wp<hg::TS<hg::i64>>(input);
    upstream.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>> value) { return hg::bundle<hg::TSB<OneValue>>("value"_n = value); }, 0.25);

    auto deferred = wiring.create_wp<hg::TS<bool>>(upstream);
    const auto input_signature = hg::Signature{
        hg::util::symbol{"materialisation"},
        {{hg::util::symbol{"value"}, &hg::TSB<OneValue>::type()}}};
    auto builder_calls = 0;
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    static_cast<hg::WiringPoint>(deferred).add_materialisation_option(
        hg::TSB<OneValue>::type(), 0.0, input_signature,
        hg::WiringMaterialisationBuilder{[&](std::span<const hg::WiringArgument> arguments) {
            ++builder_calls;
            REQUIRE(arguments.size() == 1);
            CHECK(hg::TSB<OneValue>::type().accepts(arguments[0].type()));
            CHECK(&arguments[0].type() == &arguments[0].time_series().type());
            observed_record = arguments[0].time_series().record();
            return arguments[0].time_series();
        }});

    const auto options = deferred.materialisation_options();
    REQUIRE(options.size() == 1);
    REQUIRE(options[0].input_signature != nullptr);
    CHECK(options[0].input_signature->parameter_count() == 1);
    CHECK(builder_calls == 0);

    const auto result = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());

    CHECK(builder_calls == 1);
    REQUIRE(observed_record != nullptr);
    CHECK(observed_record != static_cast<hg::WiringPoint>(upstream).record());
    CHECK(result.record() == observed_record);
    CHECK(result.record()->graph_owned());
}

TEST_CASE("typed materialisation callbacks receive the selected lazy bundle form",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto upstream = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto upstream_builder_calls = 0;
    upstream.materialisation_option<hg::TSB<OneValue>>(
        [&upstream_builder_calls](hg::wp<hg::TS<hg::i64>> value) {
            ++upstream_builder_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.25);

    const auto output = hg::node([](hg::Out<hg::TS<bool>>) {});
    const auto output_value = output();
    auto deferred = wiring.create_wp<hg::TS<bool>>(upstream);
    auto callback_calls = 0;
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    deferred.materialisation_option<hg::TS<bool>>(
        [&](hg::wp<hg::TSB<OneValue>> value) {
            ++callback_calls;
            observed_record = static_cast<hg::WiringPoint>(value).record();
            CHECK(hg::TSB<OneValue>::type().accepts(value.type()));
            CHECK(static_cast<hg::WiringPoint>(value).has_output());
            CHECK_FALSE(observed_record->has_wired_output());
            CHECK(observed_record->graph_owned());
            CHECK(hg::TS<hg::i64>::type().accepts(value->value.type()));
            return output_value;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, hg::TS<bool>::type());

    CHECK(callback_calls == 1);
    CHECK(upstream_builder_calls == 1);
    REQUIRE(observed_record != nullptr);
    CHECK(observed_record != static_cast<hg::WiringPoint>(upstream).record());
    CHECK(result.record() == static_cast<hg::WiringPoint>(output_value).record());
}

TEST_CASE("materialisation Union inputs retain the original point and its options",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"MATERIALISATION_FORM", hg::TS<hg::i64>, hg::TSB<OneValue>>;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    auto upstream = wiring.create_wp<hg::TS<hg::i64>>(input);
    upstream.materialisation_option<hg::TS<hg::i64>>(
        [](hg::wp<hg::TS<hg::i64>> value) { return static_cast<hg::WiringPoint>(value); }, 0.5);
    upstream.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>> value) { return hg::bundle<hg::TSB<OneValue>>("value"_n = value); }, 0.25);
    const auto upstream_record = static_cast<hg::WiringPoint>(upstream).record();

    auto deferred = wiring.create_wp<hg::TS<bool>>(upstream);
    auto builder_calls = 0;
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    auto observed_option_count = std::size_t{};
    deferred.materialisation_option<hg::TS<bool>>(
        [&](hg::wp<FORM> value) {
            ++builder_calls;
            observed_record = static_cast<hg::WiringPoint>(value).record();
            observed_option_count = value.materialisation_options().size();
            CHECK(&value.type() == &hg::TS<hg::i64>::type());
            return bool_input;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, hg::TS<bool>::type());

    CHECK(builder_calls == 1);
    CHECK(observed_record == upstream_record);
    CHECK(observed_option_count == 2);
    CHECK(result.record() == static_cast<hg::WiringPoint>(bool_input).record());
}

TEST_CASE("materialisation Union inputs keep their original semantic type without expanding options",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;
    using FORM = hg::Union<"MATERIALISATION_FORM", hg::TS<hg::i64>, hg::TSB<OneValue>>;

    const auto source = hg::node([](hg::Out<hg::TS<double>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    auto upstream = wiring.create_wp<hg::TS<double>>(input);
    auto upstream_builder_calls = 0;
    upstream.materialisation_option<hg::TS<hg::i64>>(
        [&upstream_builder_calls](hg::wp<hg::TS<double>>) {
            ++upstream_builder_calls;
            return hg::WiringPoint{};
        },
        0.5);
    upstream.materialisation_option<hg::TSB<OneValue>>(
        [&upstream_builder_calls](hg::wp<hg::TS<double>>) {
            ++upstream_builder_calls;
            return hg::WiringPoint{};
        },
        0.25);

    auto deferred = wiring.create_wp<hg::TS<bool>>(upstream);
    auto callback_calls = 0;
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);
    auto observed_option_count = std::size_t{};
    deferred.materialisation_option<hg::TS<bool>>(
        [&](hg::wp<FORM> value) {
            ++callback_calls;
            observed_record = static_cast<hg::WiringPoint>(value).record();
            observed_option_count = value.materialisation_options().size();
            CHECK(&value.type() == &hg::TS<double>::type());
            return bool_input;
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, hg::TS<bool>::type());

    CHECK(callback_calls == 1);
    CHECK(upstream_builder_calls == 0);
    CHECK(observed_record == static_cast<hg::WiringPoint>(upstream).record());
    CHECK(observed_option_count == 2);
    CHECK(result.record() == static_cast<hg::WiringPoint>(bool_input).record());
}

TEST_CASE("variadic materialisation callback results retain graph-owned captured arguments",
          "[feature:wiring-materialisation][feature:wiring][feature:variadic-wiring][series:tsb]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();
    auto deferred = wiring.create_wp<hg::TSB<OneValue>>("value"_n = input);
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TSB<CapturedSchema>, hg::var_kwargs> values) {
            return static_cast<hg::WiringPoint>(values);
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());

    CHECK(result.record()->graph_owned());
    const auto materialized_bundle = wiring.complete_pending_wiring(result);
    const auto bundle_output = wiring.outputs()[wiring.output_index(materialized_bundle)];
    const auto &bundle_node = wiring.nodes()[bundle_output.node_index];
    REQUIRE(bundle_node.inputs().size() == 1);
    CHECK(bundle_node.inputs()[0].index() == wiring.output_index(input));

    const auto value = result.as<hg::TSB<OneValue>>()->value;
    CHECK(&value.graph() == &wiring);
    CHECK(static_cast<hg::WiringPoint>(value).record()->graph_owned());
    CHECK(hg::TS<hg::i64>::type().accepts(value.type()));
    CHECK(wiring.output_index(value) == wiring.output_index(input));
}

TEST_CASE("signature materialisation cost counts each point and target once",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto make_deferred = [&] {
        auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
        deferred.materialisation_option<hg::TSB<OneValue>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.25);
        deferred.materialisation_option<hg::TSL<hg::TS<hg::i64>, 2>>(
            [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 1.5);
        return deferred;
    };
    const auto first = make_deferred();
    const auto second = make_deferred();
    const auto same_target_signature = hg::Signature{
        hg::util::symbol{"same_target"},
        {{hg::util::symbol{"left"}, &hg::TSB<OneValue>::type()},
         {hg::util::symbol{"right"}, &hg::TSB<OneValue>::type()}}};
    const auto same_point_arguments = std::array{
        hg::WiringArgument{static_cast<hg::WiringPoint>(first)},
        hg::WiringArgument{static_cast<hg::WiringPoint>(first)}};
    const auto distinct_point_arguments = std::array{
        hg::WiringArgument{static_cast<hg::WiringPoint>(first)},
        hg::WiringArgument{static_cast<hg::WiringPoint>(second)}};

    const auto same_point_matches = same_target_signature.match_wiring_arguments(same_point_arguments);
    const auto distinct_point_matches = same_target_signature.match_wiring_arguments(distinct_point_arguments);

    REQUIRE(same_point_matches.size() == 1);
    REQUIRE(distinct_point_matches.size() == 1);
    CHECK(same_point_matches[0].materialisation_cost == 0.25);
    CHECK(distinct_point_matches[0].materialisation_cost == 0.5);

    const auto distinct_target_signature = hg::Signature{
        hg::util::symbol{"distinct_targets"},
        {{hg::util::symbol{"left"}, &hg::TSB<OneValue>::type()},
         {hg::util::symbol{"right"}, &hg::TSL<hg::TS<hg::i64>, 2>::type()}}};
    const auto distinct_target_matches = distinct_target_signature.match_wiring_arguments(same_point_arguments);

    REQUIRE(distinct_target_matches.size() == 1);
    CHECK(distinct_target_matches[0].materialisation_cost == 1.75);
}

TEST_CASE("binding cost charges one contained and one top-level reference adaptation per input",
          "[feature:wiring-materialisation][feature:wiring][feature:reference][series:fixed-tsl]") {
    const auto source = hg::node([](hg::Out<hg::TSL<hg::TS<hg::i64>, 2>>) {});

    const auto scalar_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto scalar_input = scalar_source();
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(input)}};
    const auto scalar_arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(scalar_input)}};
    const auto not_preserved = std::array<unsigned char, 1>{0};

    auto contained_adaptation = hg::details::PreparedWiringCall{};
    contained_adaptation.resolution.signature.parameters.push_back(
        {hg::util::symbol{"values"}, &hg::TSL<hg::REF<hg::TS<hg::i64>>, 2>::type()});
    contained_adaptation.argument_forms.push_back(
        {0, &hg::TSL<hg::TS<hg::i64>, 2>::type(), nullptr, 0.0});
    contained_adaptation.preserved_inputs = not_preserved;

    CHECK(contained_adaptation.binding_cost(arguments) == 2.0);

    auto top_level_adaptation = hg::details::PreparedWiringCall{};
    top_level_adaptation.resolution.signature.parameters.push_back(
        {hg::util::symbol{"value"}, &hg::REF<hg::TS<hg::i64>>::type()});
    top_level_adaptation.argument_forms.push_back({0, &hg::TS<hg::i64>::type(), nullptr, 0.0});
    top_level_adaptation.preserved_inputs = not_preserved;

    CHECK(top_level_adaptation.binding_cost(scalar_arguments) == 1.0);
}

TEST_CASE("operator materialisation selects the cheapest concrete form and adapts it to the consumer type",
          "[feature:wiring-materialisation][feature:operator][series:reference]") {
    using namespace hg::literals;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(source());
    auto float_builder_calls = 0;
    auto bool_builder_calls = 0;
    deferred.materialisation_option<hg::TS<float>>(
        [&float_builder_calls, float_source](hg::wp<hg::TS<hg::i64>>) {
            ++float_builder_calls;
            return float_source();
        },
        0.75);
    deferred.materialisation_option<hg::TS<bool>>(
        [&bool_builder_calls, bool_source](hg::wp<hg::TS<hg::i64>>) {
            ++bool_builder_calls;
            return bool_source();
        },
        0.25);

    auto selected = std::string_view{};
    auto operation = hg::operator_<auto(hg::TS_T)->hg::TS_T>{"select_form", {"value"}};
    operation.overload(hg::graph("name"_n = "float", [&](hg::wp<hg::TS<float>> value) {
        selected = "float";
        return value;
    }));
    operation.overload(hg::graph("name"_n = "bool", [&](hg::wp<hg::TS<bool>> value) {
        selected = "bool";
        return value;
    }));

    const auto result = operation(deferred);
    REQUIRE(result.has_output());
    CHECK(selected == "bool");
    CHECK(wiring.materialisation_required(deferred, hg::TS<bool>::type()));
    CHECK(wiring.materialisation_required(static_cast<hg::WiringPoint>(result), hg::TS<bool>::type()));
    CHECK_FALSE(wiring.materialisation_required(deferred, hg::TS<float>::type()));

    auto consume = hg::node([](hg::In<hg::REF<hg::TS<bool>>>) {});
    consume(result);
    wiring.finalize();

    CHECK(float_builder_calls == 0);
    CHECK(bool_builder_calls == 1);
    const auto &consumer = wiring.nodes().back();
    REQUIRE(consumer.inputs().size() == 1);
    const auto input = consumer.inputs()[0];
    CHECK_FALSE(input.is_inherited());
    CHECK(type_cast<hg::TimeSeriesReferenceType>(wiring.outputs()[input.index()].type) != nullptr);
}

TEST_CASE("operator cache hits rebind concrete demands to the current source",
          "[feature:wiring-materialisation][feature:operator][series:reference]") {
    class PassFloat final : public hg::WiringTemplate {
      public:
        explicit PassFloat(int &requirement_calls)
            : signature_{hg::util::symbol{"pass_float"},
                         {{hg::util::symbol{"value"}, &hg::TS<float>::type()}}, hg::TS<float>::type()},
              requirement_calls_{&requirement_calls} {}

        const hg::Signature &default_signature() const final { return signature_; }
        hg::RequirementResult requirement(const hg::WiringArguments &) const final {
            ++*requirement_calls_;
            return hg::accept;
        }
        std::optional<hg::WiringPoint> wire(hg::GraphWiring &, hg::ResolvedSignature,
                                            const hg::WiringArguments &,
                                            std::span<const hg::WiringArgument> arguments) const final {
            return arguments.front().time_series();
        }

      private:
        hg::Signature signature_;
        int *requirement_calls_;
    };

    auto requirement_calls = 0;
    auto operation = hg::operator_<auto(hg::TS_T)->hg::TS_T>{"cached_form", {"value"}};
    operation.overload(PassFloat{requirement_calls});

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});
    const auto wire = [&](hg::GraphWiring &wiring) {
        const auto context = hg::WiringContext{wiring};
        const auto bind = [&] {
            auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
            deferred.materialisation_option<hg::TS<float>>(
                [float_source](hg::wp<hg::TS<hg::i64>>) { return float_source(); }, 0.0);
            const auto output = operation(deferred);
            REQUIRE(output.has_output());
            CHECK(wiring.materialisation_required(deferred, hg::TS<float>::type()));
        };
        bind();
        bind();
    };

    auto first_wiring = hg::GraphWiring{};
    wire(first_wiring);
    CHECK(requirement_calls == 1);
    first_wiring.finalize();

    auto second_wiring = hg::GraphWiring{};
    wire(second_wiring);
    CHECK(requirement_calls == 2);
    second_wiring.finalize();
}

TEST_CASE("rejected overload diagnostics distinguish offered forms from the joint binding",
          "[feature:wiring-materialisation][feature:operator][feature:wiring]") {
    class RejectFloat final : public hg::WiringTemplate {
      public:
        explicit RejectFloat(int &requirement_calls)
            : signature_{hg::util::symbol{"reject_float"},
                         {{hg::util::symbol{"value"}, &hg::TS<float>::type()}}, hg::TS<float>::type()},
              requirement_calls_{&requirement_calls} {}

        const hg::Signature &default_signature() const final { return signature_; }
        hg::RequirementResult requirement(const hg::WiringArguments &) const final {
            ++*requirement_calls_;
            return hg::reject("rejected after form matching");
        }
        std::optional<hg::WiringPoint> wire(hg::GraphWiring &, hg::ResolvedSignature,
                                            const hg::WiringArguments &,
                                            std::span<const hg::WiringArgument> arguments) const final {
            return arguments.front().time_series();
        }

      private:
        hg::Signature signature_;
        int *requirement_calls_;
    };

    using namespace hg::literals;
    auto requirement_calls = 0;
    auto operation = hg::operator_<auto(hg::TS_T)->hg::TS_T>{"form_diagnostic", {"value"}};
    operation.overload(RejectFloat{requirement_calls});

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    deferred.materialisation_option<hg::TS<float>>(
        [float_source](hg::wp<hg::TS<hg::i64>>) { return float_source(); }, 0.75);

    try {
        (void)operation(deferred);
        FAIL("expected rejected overload");
    } catch (const hg::WiringError &error) {
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
    using LIST = hg::TSL<hg::TS<hg::i64>, 1>;

    const auto source = hg::node([](hg::Out<LIST>) {});

    const auto consume = hg::node([](hg::In<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred_list = wiring.create_wp<LIST>(source());
    auto parent_demand_seen = false;
    deferred_list.materialisation_option<LIST>([&](hg::wp<LIST> value) {
        parent_demand_seen = wiring.materialisation_required(deferred_list, LIST::type());
        return value;
    }, 0.0);

    consume(deferred_list[0]);
    wiring.finalize();

    CHECK(parent_demand_seen);
}

TEST_CASE("an operator reports distinct output types from one overload as ambiguous",
          "[feature:wiring-materialisation][feature:operator][feature:type-system]") {
    using FORM = hg::Union<"AMBIGUOUS_INPUT_FORM", hg::TS<float>, hg::TS<bool>>;
    using RESULT = hg::TimeSeriesVariable<"AMBIGUOUS_RESULT">;

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(source());
    deferred.materialisation_option<hg::TS<float>>(
        [float_source](hg::wp<hg::TS<hg::i64>>) { return float_source(); }, 0.75);
    deferred.materialisation_option<hg::TS<bool>>(
        [bool_source](hg::wp<hg::TS<hg::i64>>) { return bool_source(); }, 0.25);

    auto operation = hg::operator_<auto(FORM)->RESULT>{"ambiguous_form", {"value"}};

    auto identity_function = [](hg::wp<FORM> value) -> hg::wp<RESULT> {
        return hg::wp<RESULT>::from_validated(static_cast<hg::WiringPoint>(value));
    };
    auto identity_resolver = hg::details::variable_resolver<RESULT>(
        [](const hg::WiringArguments &arguments) -> const hg::TimeSeriesType & {
            return static_cast<const hg::TimeSeriesType &>(arguments.resolved_type(0));
        });
    auto identity_signature = hg::details::make_graph_signature<decltype(identity_function)>(
        hg::util::symbol{"identity"}, std::span<const std::string_view>{},
        std::vector<hg::VariableResolver>{std::move(identity_resolver)});
    operation.overload(hg::GraphTemplate{std::move(identity_signature), std::move(identity_function)});

    try {
        (void)operation(deferred);
        FAIL("expected overload materialisation forms to resolve to different outputs");
    } catch (const hg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->ambiguous);
        CHECK(error.diagnostic()->candidates.size() == 2);
    }
}

TEST_CASE("signature completes each form match with its original arguments and resolved types",
          "[feature:wiring-materialisation][feature:wiring][schema:generic]") {
    using FORM = hg::Union<"COMPLETED_FORM", hg::TSB<OneValue>, hg::TSL<hg::TS<hg::i64>, 2>>;
    using RESOLVED_SCALAR = hg::ScalarVariable<"FORM_RESOLVED_SCALAR">;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.25);
    deferred.materialisation_option<hg::TSL<hg::TS<hg::i64>, 2>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 1.5);
    const auto threshold = hg::i64{7};
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)},
                                      hg::WiringArgument{threshold}};
    const auto descriptors = std::array{hg::details::WiringArgumentDescriptor{arguments[0]},
                                        hg::details::WiringArgumentDescriptor{arguments[1]}};
    auto resolver_calls = 0;
    auto scalar_reads = 0;
    const auto record_scalar_read = [](void *context, std::size_t index, hg::Scalar<>) {
        if (index == 1) {
            ++*static_cast<int *>(context);
        }
    };
    const auto signature = hg::Signature{
        hg::util::symbol{"complete_form"},
        {{hg::util::symbol{"value"}, &FORM::type()},
         {hg::util::symbol{"threshold"}, &hg::int64_type()}},
        {hg::VariableResolver{
            &RESOLVED_SCALAR::type(),
            [&resolver_calls](const hg::TypeMap &, const hg::WiringArguments &resolved_arguments) -> hg::TypeMapValue {
                ++resolver_calls;
                const auto threshold_value = resolved_arguments.scalar(1);
                REQUIRE(threshold_value.has_value());
                CHECK(hg::scalar_cast<hg::i64>(*threshold_value).value() == 7);
                if (&resolved_arguments.resolved_type(0) == &hg::TSB<OneValue>::type()) {
                    return hg::TimeSeriesGuarantee::ordinary;
                }
                return static_cast<const hg::Type *>(&hg::int_type());
            }}}};
    auto matches = signature.match_wiring_arguments(arguments);
    auto context_type_map = hg::TypeMap{};
    const auto argument_context =
        hg::WiringArguments{signature, context_type_map, descriptors, &scalar_reads, record_scalar_read};
    auto failed_matches = 0;
    auto completed = std::vector<hg::ResolvedWiringSignatureMatch>{};

    REQUIRE(matches.size() == 2);
    for (auto &match : matches) {
        const auto expected_type = match.argument_forms[0].offered_type;
        try {
            completed.push_back(signature.complete_wiring_match(
                std::move(match), argument_context, [&](const hg::TypeMap &type_map) {
                    const auto resolved_arguments = argument_context.with_type_map(type_map);
                    CHECK(&resolved_arguments.original_type(0) == &hg::TS<hg::i64>::type());
                    CHECK(&resolved_arguments.resolved_type(0) == expected_type);
                    static_cast<void>(type_map.find(RESOLVED_SCALAR::type()));
                }));
        } catch (const hg::TypeResolutionError &) {
            ++failed_matches;
        }
    }

    CHECK(failed_matches == 1);
    REQUIRE(completed.size() == 1);
    CHECK(completed[0].resolution.signature.parameters[0].type == &hg::TSL<hg::TS<hg::i64>, 2>::type());
    CHECK(completed[0].argument_forms[0].materialisation_target == &hg::TSL<hg::TS<hg::i64>, 2>::type());
    CHECK(completed[0].materialisation_cost == 1.5);
    CHECK(resolver_calls == 2);
    CHECK(scalar_reads == 2);
}

TEST_CASE("an open time series variable resolves from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [input](hg::wp<hg::TS<hg::i64>>) { return static_cast<hg::WiringPoint>(input); }, 4.0);
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{hg::util::symbol{"consume_any"},
                                          {{hg::util::symbol{"value"}, &hg::TS_T::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &hg::TS<hg::i64>::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &hg::TS<hg::i64>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 4.0);
    CHECK(&hg::TS_T::type().resolve(matches[0].type_map) == &hg::TS<hg::i64>::type());
    CHECK(matches[1].argument_forms[0].offered_type == &hg::TSB<OneValue>::type());
    CHECK(matches[1].argument_forms[0].materialisation_target == &hg::TSB<OneValue>::type());
    CHECK(matches[1].argument_forms[0].materialisation_cost == 0.0);
    CHECK(&hg::TS_T::type().resolve(matches[1].type_map) == &hg::TSB<OneValue>::type());

    auto conflicting_seed = hg::TypeMap{};
    const auto &variable = dynamic_cast<const hg::TypeVariableType<hg::TimeSeriesType> &>(hg::TS_T::type());
    conflicting_seed.set(variable, &hg::TS<double>::type());
    CHECK(signature.match_wiring_arguments(arguments, &conflicting_seed).empty());

    auto matching_seed = hg::TypeMap{};
    matching_seed.set(variable, &hg::TS<hg::i64>::type());
    CHECK(signature.match_wiring_arguments(arguments, &matching_seed).size() == 1);
}

TEST_CASE("independent form matches resolve variables nested in collection parameters",
          "[feature:wiring-materialisation][feature:wiring][series:fixed-tsl][schema:generic]") {
    using SOURCE = hg::TSL<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>, 2,
                           hg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = hg::TSL<hg::TS<hg::i64>, 2>;
    using PARAMETER = hg::TSL<hg::TS_T, hg::SIZE>;

    const auto source = hg::node([](hg::Out<SOURCE>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](hg::wp<SOURCE> value) { return static_cast<hg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](hg::wp<SOURCE>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{hg::util::symbol{"consume_list"},
                                          {{hg::util::symbol{"values"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    CHECK(&hg::TS_T::type().resolve(matches[0].type_map) ==
          &hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>::type());
    CHECK(&hg::TS_T::type().resolve(matches[1].type_map) == &hg::TS<hg::i64>::type());
    for (const auto &match : matches) {
        CHECK(hg::details::size_variable_instance<hg::SIZE>().resolve(match.type_map) == 2);
    }
}

TEST_CASE("a failed direct-form match does not reject an offered bundle form",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using BUNDLE = hg::TSB<OneValue>;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<BUNDLE>([](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{hg::util::symbol{"consume_bundle"},
                                          {{hg::util::symbol{"value"}, &BUNDLE::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &BUNDLE::type());
    CHECK(matches[0].argument_forms[0].materialisation_target == &BUNDLE::type());
}

TEST_CASE("independent form matches resolve a shared variable from offered forms",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto int_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto double_source = hg::node([](hg::Out<hg::TS<double>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto int_input = int_source();
    const auto double_input = double_source();
    auto int_deferred = wiring.create_wp<hg::TS<hg::i64>>(int_input);
    int_deferred.materialisation_option<hg::TS<float>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    auto double_deferred = wiring.create_wp<hg::TS<double>>(double_input);
    double_deferred.materialisation_option<hg::TS<float>>(
        [](hg::wp<hg::TS<double>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(int_deferred)},
                                      hg::WiringArgument{static_cast<hg::WiringPoint>(double_deferred)}};
    const auto signature = hg::Signature{
        hg::util::symbol{"consume_shared"},
        {{hg::util::symbol{"left"}, &hg::TS_T::type()}, {hg::util::symbol{"right"}, &hg::TS_T::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);
    const auto reversed_arguments = std::array{arguments[1], arguments[0]};
    const auto reversed_matches = signature.match_wiring_arguments(reversed_arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &hg::TS<float>::type());
    CHECK(matches[0].argument_forms[1].offered_type == &hg::TS<float>::type());
    REQUIRE(reversed_matches.size() == 1);
    CHECK(reversed_matches[0].argument_forms[0].offered_type == &hg::TS<float>::type());
    CHECK(reversed_matches[0].argument_forms[1].offered_type == &hg::TS<float>::type());
}

TEST_CASE("reference target matching resolves each selected target structure",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    using SOURCE = hg::REF<hg::TSL<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>, 2>,
                            hg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = hg::REF<hg::TSL<hg::TS<double>, 2>>;
    using PARAMETER = hg::REF<hg::TS_T>;

    const auto source = hg::node([](hg::Out<SOURCE>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](hg::wp<SOURCE> value) { return static_cast<hg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](hg::wp<SOURCE>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{hg::util::symbol{"consume_reference"},
                                          {{hg::util::symbol{"value"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(&hg::TS_T::type().resolve(matches[0].type_map) ==
          &hg::TSL<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>, 2>::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    CHECK(&hg::TS_T::type().resolve(matches[1].type_map) == &hg::TSL<hg::TS<double>, 2>::type());
}

TEST_CASE("independent form matches preserve guarantees in captured bundle schemas",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:generic]") {
    using SOURCE = hg::TSB<OneValue, hg::TimeSeriesGuarantee::final>;
    using ALTERNATIVE = hg::TSB<OneValue>;
    using PARAMETER = hg::TSB<hg::SCHEMA>;

    const auto source = hg::node([](hg::Out<SOURCE>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<SOURCE>(input);
    deferred.materialisation_option<SOURCE>([](hg::wp<SOURCE> value) { return static_cast<hg::WiringPoint>(value); },
                                            0.0);
    deferred.materialisation_option<ALTERNATIVE>([](hg::wp<SOURCE>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{hg::util::symbol{"consume_bundle"},
                                          {{hg::util::symbol{"value"}, &PARAMETER::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 2);
    CHECK(matches[0].argument_forms[0].offered_type == &SOURCE::type());
    CHECK(matches[1].argument_forms[0].offered_type == &ALTERNATIVE::type());
    const auto &schema_variable = hg::schema_type<hg::SCHEMA>::type();
    const auto *source_schema = dynamic_cast<const hg::Schema *>(&schema_variable.resolve(matches[0].type_map));
    const auto *alternative_schema = dynamic_cast<const hg::Schema *>(&schema_variable.resolve(matches[1].type_map));
    REQUIRE(source_schema != nullptr);
    REQUIRE(alternative_schema != nullptr);
    CHECK(source_schema->field(0).type == &hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>::type());
    CHECK(alternative_schema->field(0).type == &hg::TS<hg::i64>::type());
}

TEST_CASE("signature matching resolves constrained variables from selected forms",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 3.0);
    deferred.materialisation_option<hg::TS<double>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    static const auto constrained = hg::util::static_storage<hg::TypeVariableType<hg::TimeSeriesType>>{
        hg::util::symbol{"DOUBLE_SERIES"}, std::vector<const hg::TimeSeriesType *>{},
        std::vector<hg::TypeVariableType<hg::TimeSeriesType>::Constraint>{&is_double_series}};
    [[maybe_unused]] static const auto &constrained_registration =
        hg::TypeRegistry::register_static(constrained.get());
    const auto signature = hg::Signature{hg::util::symbol{"consume_double"},
                                          std::vector<hg::Parameter>{{hg::util::symbol{"value"},
                                                                      &constrained.get()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &hg::TS<double>::type());
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.0);
}

TEST_CASE("signature matching resolves a guarantee variable from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    using GUARANTEED = hg::TS_T::with<hg::guarantee_variable<"MATCH_GUARANTEE">>;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 2.0);
    deferred.materialisation_option<hg::TS<double>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};

    const auto guaranteed_signature = hg::Signature{
        hg::util::symbol{"consume_guaranteed"}, {{hg::util::symbol{"value"}, &GUARANTEED::type()}}};

    const auto guaranteed_matches = guaranteed_signature.match_wiring_arguments(arguments);

    REQUIRE(guaranteed_matches.size() == 2);
    CHECK(guaranteed_matches[0].argument_forms[0].materialisation_target == &hg::TS<hg::i64>::type());
    CHECK(&GUARANTEED::type().resolve(guaranteed_matches[0].type_map) == &hg::TS<hg::i64>::type());
    CHECK(guaranteed_matches[1].argument_forms[0].materialisation_target == &hg::TS<double>::type());
    CHECK(&GUARANTEED::type().resolve(guaranteed_matches[1].type_map) == &hg::TS<double>::type());
}

TEST_CASE("signature matching resolves a reference target from each selected form",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 2.0);
    deferred.materialisation_option<hg::TS<double>>(
        [](hg::wp<hg::TS<hg::i64>>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto reference_signature = hg::Signature{
        hg::util::symbol{"consume_reference"}, {{hg::util::symbol{"value"}, &hg::REF<hg::TS_T>::type()}}};

    const auto reference_matches = reference_signature.match_wiring_arguments(arguments);

    REQUIRE(reference_matches.size() == 2);
    CHECK(reference_matches[0].argument_forms[0].materialisation_target == &hg::TS<hg::i64>::type());
    CHECK(&hg::TS_T::type().resolve(reference_matches[0].type_map) == &hg::TS<hg::i64>::type());
    CHECK(reference_matches[1].argument_forms[0].materialisation_target == &hg::TS<double>::type());
    CHECK(&hg::TS_T::type().resolve(reference_matches[1].type_map) == &hg::TS<double>::type());
}

TEST_CASE("an ambiguous union form does not hide another valid materialisation form",
          "[feature:wiring-materialisation][feature:wiring][series:reference][schema:generic]") {
    using SOURCE = hg::REF<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>>;
    using INPUT = hg::Union<"AMBIGUOUS_REFERENCE_FORM", hg::REF<hg::TS_T>, hg::TS_T>;

    const auto source = hg::node([](hg::Out<SOURCE>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred = wiring.create_wp<SOURCE>(source());
    deferred.materialisation_option<SOURCE>(
        [](hg::wp<SOURCE> value) { return static_cast<hg::WiringPoint>(value); }, 0.0);
    deferred.materialisation_option<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>>(
        [](hg::wp<SOURCE>) { return hg::WiringPoint{}; }, 0.0);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(deferred)}};
    const auto signature = hg::Signature{
        hg::util::symbol{"consume_union"}, {{hg::util::symbol{"value"}, &INPUT::type()}}};

    const auto matches = signature.match_wiring_arguments(arguments);

    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].materialisation_target ==
          &hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>::type());
    CHECK(&hg::TS_T::type().resolve(matches[0].type_map) ==
          &hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>::type());
}

TEST_CASE("signature matching branches independent unions and unifies shared inner variables",
          "[feature:wiring-materialisation][feature:wiring][series:value][schema:generic]") {
    const auto source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto make_deferred = [&](double offset) {
        auto deferred = wiring.create_wp<hg::TS<bool>>(input);
        deferred.materialisation_option<hg::TS<hg::i64>>(
            [](hg::wp<hg::TS<bool>>) { return hg::WiringPoint{}; }, offset + 0.1);
        deferred.materialisation_option<hg::TS<double>>(
            [](hg::wp<hg::TS<bool>>) { return hg::WiringPoint{}; }, offset + 0.2);
        return deferred;
    };
    const auto left = make_deferred(0.0);
    const auto right = make_deferred(0.3);
    const auto arguments = std::array{hg::WiringArgument{static_cast<hg::WiringPoint>(left)},
                                      hg::WiringArgument{static_cast<hg::WiringPoint>(right)}};

    using LEFT_FORM = hg::Union<"left_form", hg::TS<hg::i64>, hg::TS<double>>;
    using RIGHT_FORM = hg::Union<"right_form", hg::TS<hg::i64>, hg::TS<double>>;
    const auto independent_signature = hg::Signature{
        hg::util::symbol{"independent"},
        {{hg::util::symbol{"left"}, &LEFT_FORM::type()}, {hg::util::symbol{"right"}, &RIGHT_FORM::type()}}};
    const auto independent_matches = independent_signature.match_wiring_arguments(arguments);

    REQUIRE(independent_matches.size() == 4);
    CHECK(LEFT_FORM::type().resolve(independent_matches[0].type_map).is_time_series());
    CHECK(RIGHT_FORM::type().resolve(independent_matches[0].type_map).is_time_series());
    CHECK(independent_matches[0].argument_forms[0].offered_type == &hg::TS<hg::i64>::type());
    CHECK(independent_matches[0].argument_forms[1].offered_type == &hg::TS<hg::i64>::type());
    CHECK(independent_matches[1].argument_forms[0].offered_type == &hg::TS<hg::i64>::type());
    CHECK(independent_matches[1].argument_forms[1].offered_type == &hg::TS<double>::type());

    using LEFT_COLUMN = hg::Union<"left_column", hg::TS<hg::SCALAR>>;
    using RIGHT_COLUMN = hg::Union<"right_column", hg::TS<hg::SCALAR>>;
    const auto shared_signature = hg::Signature{
        hg::util::symbol{"shared"},
        {{hg::util::symbol{"left"}, &LEFT_COLUMN::type()}, {hg::util::symbol{"right"}, &RIGHT_COLUMN::type()}}};
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
    using namespace hg::literals;
    const auto capture =
        hg::graph("name"_n = "capture", [](hg::wp<hg::TSB<CapturedSchema>, hg::var_args>) {});

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto raw_arguments = std::array{hg::WiringArgument{source()}, hg::WiringArgument{source()}};
    auto overlay = hg::details::WiringRecordOverlay{wiring};

    const auto normalized = hg::details::probe_wiring_arguments(capture.signature(), raw_arguments, overlay);
    const auto matches = capture.signature().match_wiring_arguments(normalized.arguments);

    REQUIRE(normalized.arguments.size() == capture.signature().parameter_count());
    REQUIRE(matches.size() == 1);
    CHECK(matches[0].argument_forms[0].offered_type == &normalized.arguments[0].type());
    CHECK(matches[0].argument_forms[0].materialisation_target == nullptr);
    CHECK(matches[0].argument_forms[0].materialisation_cost == 0.0);
}

TEST_CASE("materialisation collection retains context bindings for later expansion",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:fan-out]") {
    using namespace hg::literals;

    const auto first_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto second_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto consume = hg::node([](hg::In<hg::TSB<OneValue>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto first_provider = first_source();
    const auto second_provider = second_source();
    auto deferred = hg::WiringPoint{};
    {
        const auto scope = hg::with_context("value"_n = first_provider);
        deferred = wiring.create_wp<hg::TS<hg::i64>>();
    }
    auto observed_record = static_cast<const hg::details::WiringRecord *>(nullptr);

    auto input_signature = hg::Signature{
        hg::util::symbol{"materialisation"},
        {{.name = hg::util::symbol{"value"}, .type = &hg::TS<hg::i64>::type(), .context = true}}};
    {
        const auto scope = hg::with_context("value"_n = second_provider);
        deferred.add_materialisation_option(
            hg::TSB<OneValue>::type(), 0.0, std::move(input_signature),
            hg::WiringMaterialisationBuilder{[&](std::span<const hg::WiringArgument> arguments) {
                REQUIRE(arguments.size() == 1);
                observed_record = arguments[0].time_series().record();
                return hg::bundle<hg::TSB<OneValue>>("value"_n = arguments[0].time_series());
            }});
        consume(deferred);
    }
    {
        const auto scope = hg::with_context("value"_n = second_provider);
        wiring.finalize();
    }

    CHECK(observed_record == static_cast<hg::WiringPoint>(first_provider).record());
    CHECK(observed_record != static_cast<hg::WiringPoint>(second_provider).record());
}

TEST_CASE("failed transitive materialisation collection preserves committed requirements",
          "[feature:wiring-materialisation][feature:wiring][series:tsb][schema:named]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();

    auto previously_required = wiring.create_wp<hg::TS<hg::i64>>(input);
    previously_required.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>> value) { return hg::bundle<hg::TSB<OneValue>>("value"_n = value); }, 0.0);
    wiring.require_materialisation(previously_required, hg::TSB<OneValue>::type());

    auto deep = wiring.create_wp<hg::TS<hg::i64>>(input);
    deep.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TSL<hg::TS<hg::i64>, 2>>) { return hg::WiringPoint{}; }, 0.0);

    auto middle = wiring.create_wp<hg::TS<hg::i64>>(deep);
    middle.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TSB<OneValue>> value) { return value; }, 0.0);

    auto good = wiring.create_wp<hg::TS<hg::i64>>(input);
    good.materialisation_option<hg::TSB<OneValue>>(
        [](hg::wp<hg::TS<hg::i64>> value) { return hg::bundle<hg::TSB<OneValue>>("value"_n = value); }, 0.0);

    auto root = wiring.create_wp<hg::TS<bool>>(good, middle);
    root.materialisation_option<hg::TS<bool>>(
        [bool_input](hg::wp<hg::TSB<OneValue>>, hg::wp<hg::TSB<OneValue>>) { return bool_input; }, 0.0);

    CHECK(wiring.materialisation_required(previously_required, hg::TSB<OneValue>::type()));
    CHECK_THROWS_AS(wiring.require_materialisation(root, hg::TS<bool>::type()), hg::WiringError);

    CHECK(wiring.materialisation_required(previously_required, hg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(root, hg::TS<bool>::type()));
    CHECK_FALSE(wiring.materialisation_required(good, hg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(middle, hg::TSB<OneValue>::type()));
    CHECK_FALSE(wiring.materialisation_required(deep, hg::TSB<OneValue>::type()));
}

TEST_CASE("materialisation builder failures include callback and wiring path context",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto consume = hg::node([](hg::In<hg::TS<float>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(source());
    deferred.materialisation_option<hg::TS<float>>([](hg::wp<hg::TS<hg::i64>>) -> hg::WiringPoint {
        throw std::runtime_error{"builder failure"};
    }, 0.0);
    consume(deferred);

    try {
        wiring.finalize();
        FAIL("expected materialisation callback failure");
    } catch (const hg::WiringError &error) {
        INFO(error.what());
        CHECK(std::string_view{error.what()}.find("builder failure") != std::string_view::npos);
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &context) {
            return context.stage == hg::WiringFailureStage::callback &&
                   context.operation == hg::util::symbol{"materialisation"} && !context.requested_type.empty();
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &context) {
            return context.stage == hg::WiringFailureStage::requirement && context.parameter.has_value() &&
                   !context.path.empty() && !context.requested_type.empty();
        }));
    }
}

TEST_CASE("materialisation builder failures preserve operator diagnostics",
          "[feature:wiring-materialisation][feature:wiring][feature:operator]") {
    using namespace hg::literals;

    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});

    const auto consume = hg::node([](hg::In<hg::TS<float>>) {});
    auto operation = hg::operator_<auto(hg::TS<bool>)->hg::TS<float>>{"builder_operator", {"value"}};
    operation.overload(hg::graph("name"_n = "boolean", [&](hg::wp<hg::TS<bool>>) { return float_source(); }));

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
    deferred.materialisation_option<hg::TS<float>>(
        [&](hg::wp<hg::TS<hg::i64>> value) { return operation(value); }, 0.0);
    consume(deferred);

    try {
        wiring.finalize();
        FAIL("expected overload-resolution failure inside materialisation builder");
    } catch (const hg::WiringError &error) {
        REQUIRE(error.diagnostic());
        CHECK(error.diagnostic()->operator_name == hg::util::symbol{"builder_operator"});
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == hg::WiringFailureStage::callback &&
                   entry.operation == hg::util::symbol{"materialisation"};
        }));
    }
}

TEST_CASE("materialisation errors retain causes and owned contexts after graph destruction",
          "[feature:wiring-materialisation][feature:wiring]") {
    using namespace hg::literals;

    auto saved_error = std::optional<hg::WiringError>{};
    {
        const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

        const auto consume = hg::node([](hg::In<hg::TS<float>>) {});
        auto wiring = hg::GraphWiring{};
        const auto context = hg::WiringContext{wiring};
        auto deferred = wiring.create_wp<hg::TS<hg::i64>>(source());
        deferred.materialisation_option<hg::TS<float>>(
            [](hg::wp<hg::TS<hg::i64>>) -> hg::WiringPoint { throw std::runtime_error{"owned cause"}; }, 0.0);
        consume(deferred);

        try {
            wiring.finalize();
            FAIL("expected materialisation failure");
        } catch (const hg::WiringError &error) {
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
    using namespace hg::literals;

    const auto float_source = hg::node([](hg::Out<hg::TS<float>>) {});
    auto foreign_wiring = hg::GraphWiring{};
    const auto foreign_context = hg::WiringContext{foreign_wiring};
    const auto foreign_output = float_source();

    const auto invalid_graph = hg::graph("name"_n = "invalid_result", [foreign_output] { return foreign_output; });

    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    try {
        (void)invalid_graph();
        FAIL("expected a foreign wiring result");
    } catch (const hg::WiringError &error) {
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.stage == hg::WiringFailureStage::callback && !entry.operation.text().empty() &&
                   entry.path.find("invalid_result") != std::string::npos;
        }));
    }
}

TEST_CASE("shared materialisation preparation retains each consumer context once",
          "[feature:wiring-materialisation][feature:wiring][shape:fan-out]") {
    using Dependency = hg::details::MaterialisationDependency;

    auto root_preparations = 0;
    auto left_preparations = 0;
    auto right_preparations = 0;
    auto shared_preparations = 0;
    auto root_expansions = 0;
    auto left_expansions = 0;
    auto right_expansions = 0;
    auto shared_expansions = 0;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto concrete = source();
    const auto make_record = [&](int &preparations, int &expansions, bool throws,
                                 std::vector<Dependency> dependencies = {}) {
        return wiring.register_record(std::make_unique<CountingMaterialisationRecord>(
            hg::TS<hg::i64>::type(), preparations, expansions, concrete, throws, std::move(dependencies)));
    };
    const auto shared = make_record(shared_preparations, shared_expansions, true);
    const auto left = make_record(left_preparations, left_expansions, false,
                                  {{shared, &hg::TS<hg::i64>::type(), hg::util::symbol{"left_to_shared"},
                                    "root[left]/shared", std::nullopt, std::nullopt}});
    const auto right = make_record(right_preparations, right_expansions, false,
                                   {{shared, &hg::TS<hg::i64>::type(), hg::util::symbol{"right_to_shared"},
                                     "root[right]/shared", std::nullopt, std::nullopt}});
    const auto root = make_record(root_preparations, root_expansions, false,
                                  {{left, &hg::TS<hg::i64>::type(), hg::util::symbol{"root_to_left"},
                                    "root/left", std::nullopt, std::nullopt},
                                   {right, &hg::TS<hg::i64>::type(), hg::util::symbol{"root_to_right"},
                                    "root/right", std::nullopt, std::nullopt}});
    wiring.require_materialisation(root, hg::TS<hg::i64>::type(),
                                  {.path = "consumer_a[read]", .consumer = hg::util::symbol{"consumer_a"}});
    wiring.require_materialisation(root, hg::TS<hg::i64>::type(),
                                  {.path = "consumer_b[read]", .consumer = hg::util::symbol{"consumer_b"}});

    try {
        wiring.finalize();
        FAIL("expected shared materialisation expansion failure");
    } catch (const hg::WiringError &error) {
        CHECK(root_preparations == 1);
        CHECK(left_preparations == 1);
        CHECK(right_preparations == 1);
        CHECK(shared_preparations == 1);
        CHECK(root_expansions == 1);
        CHECK(left_expansions == 1);
        CHECK(right_expansions == 0);
        CHECK(shared_expansions == 1);
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == hg::util::symbol{"consumer_a"} && entry.path == "consumer_a[read]";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == hg::util::symbol{"consumer_b"} && entry.path == "consumer_b[read]";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == hg::util::symbol{"left_to_shared"} &&
                   entry.path == "root[left]/shared";
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == hg::util::symbol{"right_to_shared"} &&
                   entry.path == "root[right]/shared";
        }));
    }
}

TEST_CASE("materialisation return type failures report expected and actual types",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto integer_source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto boolean_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto producer_path = hg::WiringPath{nullptr, hg::util::symbol{"producer"}, hg::util::symbol{"create"}};
    auto deferred = [&] {
        const auto producer_context = hg::WiringContext{wiring, producer_path};
        auto point = wiring.create_wp<hg::TS<hg::i64>>(integer_source());
        point.materialisation_option<hg::TS<float>>(
            [boolean_source](hg::wp<hg::TS<hg::i64>>) { return boolean_source(); }, 0.0);
        return point;
    }();
    wiring.require_materialisation(deferred, hg::TS<float>::type(),
                                  {.path = "consumer[0]", .consumer = hg::util::symbol{"consumer"},
                                   .parameter = hg::util::symbol{"value"}});

    try {
        wiring.finalize();
        FAIL("expected incompatible materialisation result");
    } catch (const hg::WiringError &error) {
        auto expected = std::string{};
        auto actual = std::string{};
        hg::TS<float>::type().format_name(expected);
        hg::TS<bool>::type().format_name(actual);
        CHECK(std::ranges::any_of(error.contexts(), [&](const auto &entry) {
            return entry.expected_type == expected && entry.actual_type == actual;
        }));
        CHECK(std::ranges::any_of(error.contexts(), [](const auto &entry) {
            return entry.operation == hg::util::symbol{"materialisation"} &&
                   entry.path == "producer[create]";
        }));
    }
}

TEST_CASE("materialisation collection shares concrete demands with Union consumers",
          "[feature:wiring-materialisation][feature:wiring][schema:generic][shape:fan-out]") {
    using namespace hg::literals;
    using FORM = hg::Union<"SHARED_FORM", hg::TS<hg::i64>, hg::TSB<OneValue>>;

    const auto run = [&](bool concrete_first) {
        const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

        const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
        auto wiring = hg::GraphWiring{};
        const auto context = hg::WiringContext{wiring};
        const auto input = source();
        const auto bool_input = bool_source();

        auto shared = wiring.create_wp<hg::TS<hg::i64>>(input);
        auto concrete_builder_calls = 0;
        auto expression_builder_calls = 0;
        shared.materialisation_option<hg::TS<hg::i64>>(
            [&concrete_builder_calls](hg::wp<hg::TS<hg::i64>> value) {
                ++concrete_builder_calls;
                return static_cast<hg::WiringPoint>(value);
            },
            0.0);
        shared.materialisation_option<hg::TSB<OneValue>>(
            [&expression_builder_calls](hg::wp<hg::TS<hg::i64>> value) {
                ++expression_builder_calls;
                return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
            },
            0.0);

        auto union_consumer = wiring.create_wp<hg::TS<bool>>(shared);
        auto union_builder_calls = 0;
        auto union_saw_concrete_demand = false;
        union_consumer.materialisation_option<hg::TS<bool>>(
            [&](hg::wp<FORM>) {
                ++union_builder_calls;
                union_saw_concrete_demand = wiring.materialisation_required(shared, hg::TS<hg::i64>::type());
                return bool_input;
            },
            0.0);

        auto concrete_consumer = wiring.create_wp<hg::TS<bool>>(shared);
        auto concrete_consumer_calls = 0;
        concrete_consumer.materialisation_option<hg::TS<bool>>(
            [&concrete_consumer_calls, bool_input](hg::wp<hg::TS<hg::i64>>) {
                ++concrete_consumer_calls;
                return bool_input;
            },
            0.0);

        wiring.require_materialisation(union_consumer, hg::TS<bool>::type());
        wiring.require_materialisation(concrete_consumer, hg::TS<bool>::type());

        CHECK(wiring.materialisation_required(shared, hg::TS<hg::i64>::type()));
        CHECK_FALSE(wiring.materialisation_required(shared, hg::TSB<OneValue>::type()));
        CHECK(concrete_builder_calls == 0);
        CHECK(expression_builder_calls == 0);
        CHECK(union_builder_calls == 0);
        CHECK(concrete_consumer_calls == 0);

        const auto expand = [&](hg::WiringPoint point) {
            const auto result = wiring.expand_materialisation(point, hg::TS<bool>::type());
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
    hg::In<hg::TSB<OneValue>> first;
    hg::In<hg::TSB<OneValue>> second;
    hg::Out<hg::TS<hg::i64>> output;

    void evaluate() const { output.set(*first->value + *second->value); }
};

TEST_CASE("normal materialization completes and caches a lazy callback result",
          "[feature:wiring-materialisation][feature:wiring][feature:collection-change][series:tsb]"
          "[schema:named][transition:update]") {
    using namespace hg::literals;

    auto callback_calls = 0;

    const auto sink = hg::node<MaterialisationBundleSink>();

    const auto tested = [&](hg::wp<hg::TS<hg::i64>> input) {
        auto deferred = hg::WiringContext::current().wiring().create_wp<hg::TSB<OneValue>>(input);
        deferred.materialisation_option<hg::TSB<OneValue>>(
            [&callback_calls](hg::wp<hg::TS<hg::i64>> value) {
                ++callback_calls;
                return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
            },
            0.0);
        return sink(deferred, deferred);
    };

    const auto actual = hg::run_graph(tested, {7});

    CHECK(actual.equals({14}));
    CHECK(callback_calls == 1);
}

TEST_CASE("materialisation expansion permits another target on the same point",
          "[feature:wiring-materialisation][feature:wiring][schema:named][shape:chain]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto concrete_calls = 0;
    auto bundle_calls = 0;
    deferred.materialisation_option<hg::TSB<OneValue>>(
        [&bundle_calls](hg::wp<hg::TS<hg::i64>> value) {
            ++bundle_calls;
            return hg::bundle<hg::TSB<OneValue>>("value"_n = value);
        },
        0.0);
    deferred.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++concrete_calls;
            const auto alternate = wiring.expand_materialisation(deferred, hg::TSB<OneValue>::type());
            CHECK_FALSE(alternate.record()->has_wired_output());
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);

    const auto result = wiring.expand_materialisation(deferred, hg::TS<hg::i64>::type());

    CHECK(result.record() == static_cast<hg::WiringPoint>(input).record());
    CHECK(concrete_calls == 1);
    CHECK(bundle_calls == 1);
}

TEST_CASE("materialisation dependency cycles fail and failed callbacks leave no active request",
          "[feature:wiring-materialisation][feature:wiring][series:value][shape:chain]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();

    auto expanding = wiring.create_wp<hg::TS<hg::i64>>();
    auto cycle = true;
    auto expansion_calls = 0;
    expanding.materialisation_option<hg::TS<hg::i64>>([&] {
        ++expansion_calls;
        if (cycle) {
            return wiring.expand_materialisation(expanding, hg::TS<hg::i64>::type());
        }
        return static_cast<hg::WiringPoint>(input);
    }, 0.0);

    CHECK_THROWS_AS(wiring.expand_materialisation(expanding, hg::TS<hg::i64>::type()), hg::WiringError);
    cycle = false;
    CHECK(wiring.expand_materialisation(expanding, hg::TS<hg::i64>::type()).record() ==
          static_cast<hg::WiringPoint>(input).record());
    CHECK(expansion_calls == 2);

    auto first = wiring.create_wp<hg::TS<hg::i64>>();
    auto second = wiring.create_wp<hg::TS<hg::i64>>();
    first.materialisation_option<hg::TS<hg::i64>>([second] { return static_cast<hg::WiringPoint>(second); }, 0.0);
    second.materialisation_option<hg::TS<hg::i64>>([first] { return static_cast<hg::WiringPoint>(first); }, 0.0);

    CHECK_THROWS_AS(wiring.materialize(first, hg::TS<hg::i64>::type()), hg::WiringError);

    auto retry = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto fail = true;
    auto retry_calls = 0;
    retry.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++retry_calls;
            if (fail) {
                throw hg::WiringError{"injected materialisation callback failure"};
            }
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);

    CHECK_THROWS_AS(wiring.materialize(retry, hg::TS<hg::i64>::type()), hg::WiringError);
    fail = false;
    CHECK(wiring.materialize(retry, hg::TS<hg::i64>::type()).record() ==
          static_cast<hg::WiringPoint>(input).record());
    CHECK(retry_calls == 2);
}

TEST_CASE("materialisation preparation follows concrete callback inputs through free bundles",
          "[feature:wiring-materialisation][feature:wiring][series:tsb]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});

    const auto bool_source = hg::node([](hg::Out<hg::TS<bool>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto bool_input = bool_source();
    (void)wiring.complete_pending_wiring();

    auto field = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto field_builder_calls = 0;
    field.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++field_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);
    const auto bundle = hg::bundle<hg::TSB<OneValue>>("value"_n = field);

    auto root = wiring.create_wp<hg::TS<bool>>(bundle);
    auto root_builder_calls = 0;
    root.materialisation_option<hg::TS<bool>>(
        [&](hg::wp<hg::TSB<OneValue>>) {
            ++root_builder_calls;
            return static_cast<hg::WiringPoint>(bool_input);
        },
        0.0);

    wiring.require_materialisation(root, hg::TS<bool>::type());

    CHECK(wiring.materialisation_required(root, hg::TS<bool>::type()));
    CHECK(wiring.materialisation_required(bundle, hg::TSB<OneValue>::type()));
    CHECK(wiring.materialisation_required(field, hg::TS<hg::i64>::type()));
    CHECK(field_builder_calls == 0);
    CHECK(root_builder_calls == 0);

    (void)wiring.expand_materialisation(root, hg::TS<bool>::type());
    CHECK(root_builder_calls == 1);
    CHECK(field_builder_calls == 0);

    (void)wiring.complete_pending_wiring(bundle);
    CHECK(field_builder_calls == 1);
}

TEST_CASE("materialisation preparation follows collection, item, and variadic dependencies",
          "[feature:wiring-materialisation][feature:wiring][series:fixed-tsl][series:tsb]") {
    using namespace hg::literals;

    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    (void)wiring.complete_pending_wiring();

    auto field = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto field_builder_calls = 0;
    field.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++field_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);

    const auto list = hg::tsl(field);
    CHECK_FALSE(wiring.materialisation_required(field, hg::TS<hg::i64>::type()));
    wiring.require_materialisation(list, list.type());
    CHECK(wiring.materialisation_required(field, hg::TS<hg::i64>::type()));
    CHECK(field_builder_calls == 0);
    (void)wiring.complete_pending_wiring(list);
    CHECK(field_builder_calls == 1);

    auto list_source = hg::node([](hg::Out<hg::REF<hg::TSL<hg::TS<hg::i64>, 1>>>) {});
    const auto reference_list = list_source();
    const auto selected = hg::details::time_series_list_item(reference_list, 0);
    CHECK_FALSE(wiring.materialisation_required(selected, selected.type()));
    CHECK_FALSE(wiring.materialisation_required(reference_list, hg::TSL<hg::TS<hg::i64>, 1>::type()));
    wiring.require_materialisation(selected, selected.type());
    CHECK(wiring.materialisation_required(reference_list, hg::TSL<hg::TS<hg::i64>, 1>::type()));

    auto bundle_source = hg::node([](hg::Out<hg::TSB<OneValue>>) {});
    const auto source_bundle = bundle_source();
    const auto selected_bundle_field = source_bundle->value;
    CHECK_FALSE(wiring.materialisation_required(source_bundle, hg::TSB<OneValue>::type()));
    wiring.require_materialisation(selected_bundle_field, hg::TS<hg::i64>::type());
    CHECK(wiring.materialisation_required(source_bundle, hg::TSB<OneValue>::type()));

    auto variadic_field = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto variadic_builder_calls = 0;
    variadic_field.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++variadic_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);
    auto variadic_argument = hg::WiringArgument{variadic_field};
    variadic_argument.set_variadic_name(hg::util::symbol{"value"}, hg::details::VariadicMode::named);
    const auto *operations = hg::details::variadic_wiring_operations<hg::TSB<OneValue>>();
    REQUIRE(operations != nullptr);
    auto variadic = wiring.register_record(std::make_unique<hg::details::VariadicWiringRecord>(
        hg::TSB<OneValue>::type(), hg::details::VariadicMode::named, *operations,
        std::vector<hg::WiringArgument>{std::move(variadic_argument)}, nullptr));
    CHECK_FALSE(wiring.materialisation_required(variadic_field, hg::TS<hg::i64>::type()));
    wiring.require_materialisation(variadic, variadic.type());
    CHECK(wiring.materialisation_required(variadic_field, hg::TS<hg::i64>::type()));
    CHECK(variadic_builder_calls == 0);
    (void)wiring.complete_pending_wiring(variadic);
    CHECK(variadic_builder_calls == 1);

    auto variadic_list_field = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto variadic_list_builder_calls = 0;
    variadic_list_field.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++variadic_list_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);
    auto variadic_list_argument = hg::WiringArgument{variadic_list_field};
    const auto *list_operations =
        hg::details::variadic_wiring_operations<hg::TSL<hg::TS<hg::i64>, 1>>();
    REQUIRE(list_operations != nullptr);
    auto variadic_list = wiring.register_record(std::make_unique<hg::details::VariadicWiringRecord>(
        hg::TSL<hg::TS<hg::i64>, 1>::type(), hg::details::VariadicMode::positional, *list_operations,
        std::vector<hg::WiringArgument>{std::move(variadic_list_argument)}, nullptr));
    CHECK_FALSE(wiring.materialisation_required(variadic_list_field, hg::TS<hg::i64>::type()));
    wiring.require_materialisation(variadic_list, variadic_list.type());
    CHECK(wiring.materialisation_required(variadic_list_field, hg::TS<hg::i64>::type()));
    CHECK(variadic_list_builder_calls == 0);
    (void)wiring.complete_pending_wiring(variadic_list);
    CHECK(variadic_list_builder_calls == 1);

    auto direct_list_field = wiring.create_wp<hg::TS<hg::i64>>(input);
    auto direct_list_builder_calls = 0;
    direct_list_field.materialisation_option<hg::TS<hg::i64>>(
        [&](hg::wp<hg::TS<hg::i64>> value) {
            ++direct_list_builder_calls;
            return static_cast<hg::WiringPoint>(value);
        },
        0.0);
    const auto direct_list = hg::tsl(direct_list_field);
    auto direct_variadic = wiring.register_record(std::make_unique<hg::details::VariadicWiringRecord>(
        direct_list, hg::details::VariadicMode::positional, *list_operations));
    CHECK_FALSE(wiring.materialisation_required(direct_list_field, hg::TS<hg::i64>::type()));
    wiring.require_materialisation(direct_variadic, direct_variadic.type());
    CHECK(wiring.materialisation_required(direct_list_field, hg::TS<hg::i64>::type()));
    CHECK(direct_list_builder_calls == 0);
    (void)wiring.complete_pending_wiring(direct_variadic);
    CHECK(direct_list_builder_calls == 1);
}

TEST_CASE("materialisation preparation rejects an incompatible target for a default materialisation",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(wiring.require_materialisation(input, hg::TS<bool>::type()), hg::WiringError);
}

TEST_CASE("a compatible direct point is its own selected materialisation form",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64, hg::TimeSeriesGuarantee::final>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();
    const auto node_count = wiring.nodes().size();

    const auto selected = wiring.materialisation_form(input, hg::TS<hg::i64>::type());

    CHECK(selected.record() == static_cast<hg::WiringPoint>(input).record());
    CHECK(wiring.nodes().size() == node_count);
}

TEST_CASE("materialisation form rejects incompatible direct and foreign points",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();

    CHECK_THROWS_AS(wiring.materialisation_form(input, hg::TS<bool>::type()), hg::WiringError);

    auto foreign_wiring = hg::GraphWiring{};
    auto foreign_point = hg::WiringPoint{};
    {
        const auto foreign_context = hg::WiringContext{foreign_wiring};
        foreign_point = source();
    }

    try {
        (void)wiring.materialisation_form(foreign_point, hg::TS<hg::i64>::type());
        FAIL("expected a foreign graph wiring point to be rejected");
    } catch (const hg::WiringError &error) {
        CHECK(std::string_view{error.what()} == "a selected materialisation form requires a graph-owned point");
    }
}

TEST_CASE("a deferred point requires an advertised selected materialisation form",
          "[feature:wiring-materialisation][feature:wiring]") {
    const auto source = hg::node([](hg::Out<hg::TS<hg::i64>>) {});
    auto wiring = hg::GraphWiring{};
    const auto context = hg::WiringContext{wiring};
    const auto input = source();

    const auto deferred = wiring.create_wp<hg::TS<hg::i64>>(input);
    try {
        (void)wiring.materialisation_form(deferred, hg::TS<hg::i64>::type());
        FAIL("expected an unadvertised deferred materialisation form to be rejected");
    } catch (const hg::WiringError &error) {
        CHECK(std::string_view{error.what()} == "a selected materialisation form must name an advertised target");
    }
}

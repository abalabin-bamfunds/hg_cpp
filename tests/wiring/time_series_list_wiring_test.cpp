#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

class list_source_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const auto &list = bg::TSL<bg::TS<float>, 3>::type();
        static const auto value = bg::Signature{bg::util::symbol{"list_source"}, {}, list};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::TimeSeriesRuntimeType *
    output_runtime_type(const bg::ResolvedSignature &, std::span<const bg::ScalarRuntimeType *const>,
                        std::span<const bg::TimeSeriesRuntimeType *const>) const noexcept override {
        return nullptr;
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

class float_sink_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const auto &input = bg::TS<float>::type();
        static const auto value = bg::Signature{bg::util::symbol{"float_sink"}, {{bg::util::symbol{"input"}, &input}}};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::TimeSeriesRuntimeType *
    output_runtime_type(const bg::ResolvedSignature &, std::span<const bg::ScalarRuntimeType *const>,
                        std::span<const bg::TimeSeriesRuntimeType *const>) const noexcept override {
        return nullptr;
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

class list_sink_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const auto &list = bg::TSL<bg::TS<float>, 2>::type();
        static const auto value = bg::Signature{bg::util::symbol{"list_sink"}, {{bg::util::symbol{"input"}, &list}}};
        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::TimeSeriesRuntimeType *
    output_runtime_type(const bg::ResolvedSignature &, std::span<const bg::ScalarRuntimeType *const>,
                        std::span<const bg::TimeSeriesRuntimeType *const>) const noexcept override {
        return nullptr;
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

} // namespace

TEST_CASE("graph wiring records fixed time series list item selection",
          "[wiring][tsl][series:fixed-tsl][feature:wiring]") {
    const auto source = list_source_template{};
    const auto consume = float_sink_template{};
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        const auto list = bg::wp<bg::TSL<bg::TS<float>, 3>>{*source()};
        (void)consume(list[1]);
        (void)consume(list[1]);
    }

    const auto points = wiring.wiring_points();
    REQUIRE(points.size() == 2);

    const auto &list = points[0];
    const auto &item = points[1];
    CHECK(list.parent == bg::WiringPointRecord::npos);
    CHECK(item.parent == 0);
    CHECK(item.item == 1);
    CHECK(item.node_index == list.node_index);

    const auto *list_type = dynamic_cast<const bg::TimeSeriesListType *>(list.type);
    REQUIRE(list_type != nullptr);
    CHECK(item.type == &list_type->item_type());

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes()[1].input_sources()[0] == 1);
    CHECK(wiring.nodes()[2].input_sources()[0] == 1);
}

TEST_CASE("fixed time series list selection checks its index", "[wiring][tsl][series:fixed-tsl][feature:wiring]") {
    const auto source = list_source_template{};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto list = bg::wp<bg::TSL<bg::TS<float>, 3>>{*source()};

    CHECK_THROWS_AS(list[3], std::out_of_range);
}

TEST_CASE("a free fixed time series list is materialized when consumed",
          "[wiring][tsl][series:fixed-tsl][free][representation:free][feature:wiring]"
          "[binding:representation]") {
    const auto first_source = list_source_template{};
    const auto second_source = list_source_template{};
    const auto sink = list_sink_template{};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = bg::wp<bg::TSL<bg::TS<float>, 3>>{*first_source()}[0];
    const auto second = bg::wp<bg::TSL<bg::TS<float>, 3>>{*second_source()}[1];
    const auto list = bg::tsl(first, second);
    REQUIRE(wiring.nodes().size() == 2);
    (void)sink(list);

    REQUIRE(wiring.nodes().size() == 4);
    CHECK(wiring.nodes()[2].input_sources().size() == 2);
}

TEST_CASE("a free fixed time series list reuses materialization for the same input type",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]"
          "[binding:representation][shape:fan-out]") {
    const auto first_source = list_source_template{};
    const auto second_source = list_source_template{};
    const auto sink = list_sink_template{};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto first = bg::wp<bg::TSL<bg::TS<float>, 3>>{*first_source()}[0];
    const auto second = bg::wp<bg::TSL<bg::TS<float>, 3>>{*second_source()}[1];
    const auto list = bg::tsl(first, second);
    (void)sink(list);
    (void)sink(list);

    REQUIRE(wiring.nodes().size() == 5);
    CHECK(wiring.nodes()[3].input_sources()[0] == wiring.nodes()[4].input_sources()[0]);
}

TEST_CASE("selecting a free fixed time series list item returns its source without materialization",
          "[wiring][tsl][series:fixed-tsl][representation:free][feature:wiring]") {
    const auto source = list_source_template{};
    const auto sink = float_sink_template{};
    auto wiring = bg::GraphWiring{};

    const auto context = bg::WiringContext{wiring};
    const auto source_list = bg::wp<bg::TSL<bg::TS<float>, 3>>{*source()};
    const auto first = source_list[0];
    const auto list = bg::tsl(first, source_list[1]);
    (void)sink(first);
    (void)sink(list[0]);

    REQUIRE(wiring.nodes().size() == 3);
    CHECK(wiring.nodes()[1].input_sources()[0] == wiring.nodes()[2].input_sources()[0]);
}

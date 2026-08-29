#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/list/type.hpp>
#include <bg/time_series/type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

namespace {

class list_source_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const auto &item = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
        static const auto &list = bg::time_series_list_type(item, 3, bg::TimeSeriesGuarantee::ordinary);
        static const auto value = bg::Signature{bg::util::symbol{"list_source"}, {}, list};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const>) const noexcept override {
        return nullptr;
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

class float_sink_template final : public bg::NodeTemplate {
  public:
    [[nodiscard]] const bg::Signature &signature() const override {
        static const auto &input = bg::time_series_value_type(bg::float_type(), bg::TimeSeriesGuarantee::ordinary);
        static const auto value = bg::Signature{bg::util::symbol{"float_sink"}, {{bg::util::symbol{"input"}, &input}}};

        return value;
    }

    [[nodiscard]] bg::EvaluationFunction evaluation_function(const bg::ResolvedSignature &) const noexcept override {
        return &evaluate;
    }

    [[nodiscard]] const bg::TimeSeriesStorageRepresentation *
    output_storage_representation(const bg::ResolvedSignature &,
                                  std::span<const bg::ScalarStorageRepresentation *const>,
                                  std::span<const bg::TimeSeriesStorageRepresentation *const>) const noexcept override {
        return nullptr;
    }

  private:
    static void evaluate(const bg::CompiledNode &, void *, bg::EvaluationContext *) {}
};

} // namespace

TEST_CASE("graph wiring records fixed time series list item selection", "[wiring][tsl]") {
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

TEST_CASE("fixed time series list selection checks its index", "[wiring][tsl]") {
    const auto source = list_source_template{};
    auto wiring = bg::GraphWiring{};
    const auto context = bg::WiringContext{wiring};
    const auto list = bg::wp<bg::TSL<bg::TS<float>, 3>>{*source()};

    CHECK_THROWS_AS(list[3], std::out_of_range);
}

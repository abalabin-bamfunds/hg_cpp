#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node.hpp>
#include <bg/time_series/item_id.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

namespace {

using IntDictionary = bg::TSD<std::int64_t, bg::TS<int>>;

struct dict_source_definition {
    bg::Out<IntDictionary> output;

    void evaluate() const {}
};

struct key_sink_definition {
    bg::In<bg::TSS<std::int64_t>> input;

    void evaluate() const {}
};

} // namespace

TEST_CASE("dictionary key selection records its shared key set", "[wiring][tsd][series:tsd][feature:wiring]") {
    const auto source = bg::node<dict_source_definition>();
    const auto sink = bg::node<key_sink_definition>();
    auto wiring = bg::GraphWiring{};

    {
        const auto context = bg::WiringContext{wiring};
        sink(bg::keys(source()));
    }

    REQUIRE(wiring.nodes().size() == 2);
    REQUIRE(wiring.wiring_points().size() == 2);
    CHECK(wiring.wiring_points()[1].parent == 0);
    CHECK(wiring.wiring_points()[1].item == bg::key_set_item_id);
}

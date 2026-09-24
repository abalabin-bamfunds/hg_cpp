#include <bg/graph.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

class MeshOverload final : public WiringTemplate {
  public:
    template <typename Pattern>
    MeshOverload(const Pattern &pattern, bool &selected)
        : graph_type_{std::make_shared<GraphArgumentType>(
              std::make_shared<const Pattern>(pattern))},
          signature_{
              util::symbol{"mesh_overload"},
              {{util::symbol{"function"}, graph_type_.get()},
               {util::symbol{"values"}, &TSD<std::int64_t, TS<int>>::type()}},
              TSD<std::int64_t, TS<int>>::type()},
          selected_{&selected} {}

    const Signature &signature() const final { return signature_; }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const TypeMap &,
                                    std::span<const WiringArgument> arguments) const final {
        *selected_ = true;
        return arguments[1].time_series();
    }

    std::shared_ptr<const GraphArgumentType> graph_type_;
    Signature signature_;
    bool *selected_;
};

TEST_CASE("mesh dependency selection requires mesh branch wiring",
          "[feature:mesh][feature:wiring][feature:operator][shape:nested]") {
    testing::test_wiring([] {
        CHECK_THROWS_AS(mesh()[1], WiringError);
    });
}

TEST_CASE("mesh dependency selection accepts a scalar key",
          "[feature:mesh][feature:wiring][feature:operator][shape:nested][scalar:integer]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto consume = node([](In<TS<int>, optional>) {});

    testing::test_wiring([&] {
        (void)mesh(
            [&](auto value) {
                consume(mesh()[std::int64_t{1}]);
                return value;
            },
            values());
    });
}

TEST_CASE("mesh selects named enclosing scopes",
          "[feature:mesh][feature:wiring][feature:operator][shape:deeply-nested]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto consume = node([](In<TS<int>, optional>) {});

    testing::test_wiring([&] {
        (void)mesh(
            "name"_n = "outer",
            [&](auto value) {
                const auto inner = mesh(
                    "name"_n = "inner",
                    [&](auto inner) {
                        consume(mesh("outer")[std::int64_t{1}]);
                        return inner;
                    },
                    values());
                return inner[std::int64_t{1}].template as<TS<int>>();
            },
            values());
    });
}

TEST_CASE("mesh reports invalid enclosing names",
          "[feature:mesh][feature:wiring][feature:operator][shape:deeply-nested]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});

    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)mesh(
                "name"_n = "outer",
                [&](auto value) {
                    (void)mesh("missing")[std::int64_t{1}];
                    return value;
                },
                values());
        }),
        "no enclosing mesh is named 'missing'");

    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)mesh(
                "name"_n = "same",
                [&](auto value) {
                    const auto inner = mesh(
                        "name"_n = "same", [](auto item) { return item; }, values());
                    return inner[std::int64_t{1}].template as<TS<int>>();
                },
                values());
        }),
        "an enclosing mesh already has the name 'same'");
}

TEST_CASE("mesh accepts fully runtime graph and argument values",
          "[feature:mesh][feature:operator][feature:graph-argument][feature:wiring]"
          "[series:tsd][definition:runtime]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto identity = graph(
        "name"_n = "runtime_mesh_identity",
        [](wp<TS<int>> value) { return value; });
    const auto sink = graph(
        "name"_n = "runtime_mesh_sink",
        [](wp<TS<int>>) {});

    testing::test_wiring([&] {
        const auto arguments = std::vector<MapArgument>{{WiringArgument{values()}}};
        const WiringTemplate &runtime_identity = identity;
        const WiringTemplate &runtime_sink = sink;

        const auto result = mesh(runtime_identity, std::span<const MapArgument>{arguments});
        const auto &expected = TSD<
            std::int64_t,
            REF<TS<int>, TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
        CHECK_FALSE(mesh(runtime_sink, std::span<const MapArgument>{arguments}).has_output());
    });
}

TEST_CASE("mesh selects a custom operator overload",
          "[feature:mesh][feature:operator][feature:graph-argument][feature:wiring]"
          "[series:tsd]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto identity = graph(
        "name"_n = "custom_mesh_identity",
        [](wp<TS<int>> value) { return value; });

    auto selected = false;
    auto operation = std::remove_cvref_t<decltype(mesh)>{"mesh", {"graph", "args", "kwargs"}};
    operation.overload(MeshOverload{identity, selected});

    testing::test_wiring([&] {
        const auto input = values();
        const auto result = operation(identity, input);

        CHECK(selected);
        CHECK(&result.type() == &input.type());
    });
}

TEST_CASE("mesh resolves untyped arguments and selected outputs",
          "[feature:mesh][feature:operator][feature:wiring][series:tsd]"
          "[declaration:runtime]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto consume = node([](In<TS<int>>) {});
    const auto identity = graph(
        "name"_n = "untyped_mesh_identity",
        [consume](wp<TS<int>> value) {
            consume(mesh()[std::int64_t{1}]);
            return value;
        });

    testing::test_wiring([&] {
        const auto result = mesh(identity, wp<>{values()});
        const auto &expected = TSD<
            std::int64_t,
            REF<TS<int>, TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
    });
}

TEST_CASE("mesh rejects a selected type incompatible with its branch output",
          "[feature:mesh][feature:operator][feature:wiring][series:tsd]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto consume = node([](In<TS<float>>) {});
    const auto incompatible = graph(
        "name"_n = "incompatible_mesh_selection",
        [consume](wp<TS<int>> value) {
            consume(mesh()[std::int64_t{1}]);
            return value;
        });

    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)mesh(incompatible, values());
        }),
        "mesh()[key] requests a type that does not accept the mesh branch output");
}

TEST_CASE("mesh diagnostics identify mesh arguments",
          "[feature:mesh][feature:operator][feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<std::int64_t, TS<int>>>) {});
    const auto floating_point_keys = node([](Out<TSD<float, TS<int>>>) {});

    CHECK_THROWS_WITH(
        testing::test_wiring([&] {
            (void)mesh([](auto left, auto) { return left; }, integers(),
                       floating_point_keys());
        }),
        "all mesh TSD and TSS arguments must have the same key type");
}

} // namespace

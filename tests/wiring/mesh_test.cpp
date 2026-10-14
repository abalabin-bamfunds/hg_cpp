#include <bg/graph.hpp>
#include <bg/graph/graph_argument.hpp>
#include <bg/mesh.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/testing/test_wiring.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace bg;
using namespace bg::literals;

using RESULT = TimeSeriesVariable<"RESULT">;

class MeshOverload final : public WiringTemplate {
  public:
    template <typename Pattern>
    MeshOverload(const Pattern &pattern, bool &selected)
        : graph_constraint_{std::make_shared<GraphArgumentConstraint>(
              std::make_shared<const Pattern>(pattern))},
          signature_{make_signature(graph_constraint_)},
          selected_{&selected} {}

    const Signature &signature() const final { return signature_; }

  private:
    static Signature make_signature(std::shared_ptr<const GraphArgumentConstraint> graph_constraint) {
        auto function = Parameter{util::symbol{"function"}, &GraphArgument::type()};
        function.graph_constraint = std::move(graph_constraint);
        return Signature{util::symbol{"mesh_overload"},
                         {std::move(function),
                          {util::symbol{"values"}, &TSD<std::int64_t, TS<i64>>::type()}},
                         TSD<std::int64_t, TS<i64>>::type()};
    }

    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        *selected_ = true;
        return arguments[1].time_series();
    }

    std::shared_ptr<const GraphArgumentConstraint> graph_constraint_;
    Signature signature_;
    bool *selected_;
};

TEST_CASE("mesh dependencies require mesh branch wiring",
          "[feature:mesh][feature:wiring][feature:operator][shape:nested]") {
    testing::test_wiring([] { CHECK_THROWS_AS(mesh()[1], WiringError); });
}

TEST_CASE("mesh dependencies accept a scalar key",
          "[feature:mesh][feature:wiring][feature:operator][shape:nested][scalar:integer]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto consume = node([](In<TS<i64>, optional>) {});

    testing::test_wiring([&] {
        (void)mesh(
            [&](auto value) {
                consume(mesh()[std::int64_t{1}]);
                return value;
            },
            values());
    });
}

class CountingMeshBranch final : public WiringTemplate {
  public:
    explicit CountingMeshBranch(std::size_t &requirement_calls)
        : requirement_calls_{&requirement_calls} {}

    const Signature &signature() const final {
        static const auto value = Signature{
            util::symbol{"counting_mesh_branch"},
            {{util::symbol{"value"}, &TS<i64>::type()}},
            TS<i64>::type(),
        };
        return value;
    }

    RequirementResult requirement(const WiringArguments &) const final {
        ++*requirement_calls_;
        return accept;
    }

  private:
    std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                    std::span<const WiringArgument> arguments) const final {
        const auto selected = mesh()[std::int64_t{1}];
        CHECK(&selected.type() == &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
        return arguments.front().time_series();
    }

    std::size_t *requirement_calls_;
};

TEST_CASE("mesh resolves and checks its branch graph once",
          "[feature:mesh][feature:wiring][feature:operator][shape:nested]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    auto requirement_calls = std::size_t{};
    const auto branch = CountingMeshBranch{requirement_calls};

    testing::test_wiring([&] {
        const auto arguments = std::vector<MapArgument>{{WiringArgument{values()}}};
        (void)mesh(branch, std::span<const MapArgument>{arguments});
    });

    CHECK(requirement_calls == 1);
}

TEST_CASE("mesh selects named enclosing scopes",
          "[feature:mesh][feature:wiring][feature:operator][shape:deeply-nested]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto consume = node([](In<TS<i64>, optional>) {});

    testing::test_wiring([&] {
        (void)mesh(
            "name"_n = "outer",
            [&](auto) {
                const auto inner = mesh(
                    "name"_n = "inner",
                    [&](auto inner) {
                        consume(mesh("outer")[std::int64_t{1}]);
                        return inner;
                    },
                    values());
                return inner[std::int64_t{1}].template as<TS<i64>>();
            },
            values());
    });
}

TEST_CASE("mesh reports invalid enclosing names",
          "[feature:mesh][feature:wiring][feature:operator][shape:deeply-nested]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});

    CHECK_THROWS_WITH(testing::test_wiring([&] {
                          (void)mesh(
                              "name"_n = "outer",
                              [&](auto value) {
                                  (void)mesh("missing")[std::int64_t{1}];
                                  return value;
                              },
                              values());
                      }),
                      Catch::Matchers::ContainsSubstring("no enclosing mesh is named 'missing'"));

    CHECK_THROWS_WITH(testing::test_wiring([&] {
                          (void)mesh(
                              "name"_n = "same",
                              [&](auto) {
                                  const auto inner = mesh("name"_n = "same", [](auto item) { return item; }, values());
                                  return inner[std::int64_t{1}].template as<TS<i64>>();
                              },
                              values());
                      }),
                      Catch::Matchers::ContainsSubstring("an enclosing mesh already has the name 'same'"));
}

TEST_CASE("mesh accepts fully runtime graph and argument values",
          "[feature:mesh][feature:operator][feature:graph-argument][feature:wiring]"
          "[series:tsd][definition:runtime]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto identity = graph("name"_n = "runtime_mesh_identity", [](wp<TS<i64>> value) { return value; });
    const auto sink = graph("name"_n = "runtime_mesh_sink", [](wp<TS<i64>>) {});

    testing::test_wiring([&] {
        const auto arguments = std::vector<MapArgument>{{WiringArgument{values()}}};
        const WiringTemplate &runtime_identity = identity;
        const WiringTemplate &runtime_sink = sink;

        const auto result = mesh(runtime_identity, std::span<const MapArgument>{arguments});
        const auto &expected = TSD<std::int64_t, REF<TS<i64>, TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
        CHECK_FALSE(mesh(runtime_sink, std::span<const MapArgument>{arguments}).has_output());
    });
}

TEST_CASE("mesh selects a custom operator overload",
          "[feature:mesh][feature:operator][feature:graph-argument][feature:wiring]"
          "[series:tsd]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto identity = graph("name"_n = "custom_mesh_identity", [](wp<TS<i64>> value) { return value; });

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
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto consume = node([](In<TS<i64>>) {});
    const auto identity = graph("name"_n = "untyped_mesh_identity", [consume](wp<TS<i64>> value) {
        consume(mesh()[std::int64_t{1}]);
        return value;
    });

    testing::test_wiring([&] {
        const auto result = mesh(identity, wp<>{values()});
        const auto &expected = TSD<std::int64_t, REF<TS<i64>, TimeSeriesGuarantee::deduplicated>>::type();

        CHECK(&result.type() == &expected);
    });
}

TEST_CASE("an explicit mesh output type resolves a raw branch lambda before selection wiring",
          "[feature:mesh][feature:operator][feature:wiring][feature:type-system]"
          "[declaration:generic][series:tsd]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});

    testing::test_wiring([&] {
        const auto result = mesh[type<RESULT> = type<TS<i64>>](
            [](auto value) -> wp<RESULT> {
                const auto selected = mesh()[std::int64_t{1}];
                CHECK(&selected.type() == &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());
                return wp<RESULT>{value};
            },
            values());

        CHECK(&result.type() ==
              &TSD<std::int64_t, REF<TS<i64>, TimeSeriesGuarantee::deduplicated>>::type());
    });
}

TEST_CASE("mesh selection has the branch output item type before it is consumed",
          "[feature:mesh][feature:operator][feature:wiring][series:tsd]") {
    const auto values = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto consume = node([](In<TS<i64>>) {});
    const auto branch = graph("name"_n = "exact_mesh_selection", [consume](wp<TS<i64>> value) {
        const auto selected = mesh()[std::int64_t{1}];
        CHECK(&selected.type() == &REF<TS<i64>, TimeSeriesGuarantee::deduplicated>::type());

        consume(selected);
        return value;
    });

    testing::test_wiring([&] { (void)mesh(branch, values()); });
}

TEST_CASE("mesh diagnostics identify mesh arguments", "[feature:mesh][feature:operator][feature:wiring][series:tsd]") {
    const auto integers = node([](Out<TSD<std::int64_t, TS<i64>>>) {});
    const auto floating_point_keys = node([](Out<TSD<float, TS<i64>>>) {});

    CHECK_THROWS_WITH(testing::test_wiring(
                          [&] { (void)mesh([](auto left, auto) { return left; }, integers(), floating_point_keys()); }),
                      "all mesh TSD and TSS arguments must have the same key type");
}

} // namespace

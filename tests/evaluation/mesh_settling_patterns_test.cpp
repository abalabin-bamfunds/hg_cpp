#include <hg/mesh.hpp>
#include <hg/node.hpp>
#include <hg/operators.hpp>
#include <hg/run_graph.hpp>
#include <hg/switch.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>
#include <hg/time_series/tss/tss_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace hg;
using namespace hg::literals;

namespace {

TEST_CASE("mesh retargets to an existing quiescent dependency branch",
          "[feature:mesh][feature:nested-graph][series:tsd][transition:update]") {
    const auto add_dependency = node([](i64 key, std::optional<i64> dependency) {
        return key + dependency.value_or(0);
    });

    const auto chain = graph([&](wp<TS<i64>> key, wp<TS<i64>> link) -> wp<TS<i64>> {
        return add_dependency(key, mesh()[link]);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> links) {
        return mesh(chain, links, "__key_arg__"_n = 0)[i64{2}];
    };

    CHECK(run_graph(tested, {{{1, 0}, {2, 0}}, {{2, 1}}}).equals({2, 3}));
}

TEST_CASE("mesh re-reads a quiescent dependency when its link ticks again",
          "[feature:mesh][feature:nested-graph][series:tsd][transition:update]") {
    const auto add_dependency = node([](i64 key, std::optional<i64> dependency) {
        return key + dependency.value_or(0);
    });

    const auto chain = graph([&](wp<TS<i64>> key, wp<TS<i64>> link) -> wp<TS<i64>> {
        return add_dependency(key, mesh()[link]);
    });

    const auto tested = [&](wp<TSD<i64, TS<i64>>> links) {
        return mesh(chain, links, "__key_arg__"_n = 0)[i64{2}];
    };

    CHECK(run_graph(tested, {{{1, 0}, {2, 1}}, {{2, 1}}}).equals({3, literal::SKIP}));
}

TEST_CASE("mesh settles branches after a switch adds dependencies",
          "[feature:mesh][feature:switch][feature:nested-graph][series:tss][shape:deeply-nested]"
          "[transition:addition][transition:update]") {
    const auto add = node([](i64 left, i64 right) { return left + right; });

    const auto root = [&](wp<TS<i64>>, wp<TS<i64>>) -> wp<TS<i64>> {
        return add(mesh()[i64{1}], mesh()[i64{2}]);
    };

    const auto middle_initial = [](wp<TS<i64>>, wp<TS<i64>>) -> wp<TS<i64>> {
        return operators::const_(i64{10}).as<TS<i64>>();
    };

    const auto middle_expanded = [&](wp<TS<i64>>, wp<TS<i64>>) -> wp<TS<i64>> {
        return add(mesh()[i64{11}], mesh()[i64{12}]);
    };

    const auto middle = [&](wp<TS<i64>> key, wp<TS<i64>> phase) -> wp<TS<i64>> {
        return switch_(phase,
                       cases{case_(i64{0}, middle_initial), case_(DEFAULT, middle_expanded)},
                       key, phase)
            .as<TS<i64>>();
    };

    const auto leaf = [&](wp<TS<i64>> key, wp<TS<i64>> phase) -> wp<TS<i64>> {
        return add(key, phase);
    };

    const auto cascade = graph([&](wp<TS<i64>> key, wp<TS<i64>> phase) -> wp<TS<i64>> {
        return switch_(key,
                       cases{case_(i64{0}, root), case_(i64{1}, middle), case_(DEFAULT, leaf)},
                       key, phase)
            .as<TS<i64>>();
    });

    const auto tested = [&](wp<TSS<i64>> keys, wp<TS<i64>> phase) {
        return mesh(cascade, phase, keys, "__key_arg__"_n = 0)[i64{0}];
    };

    CHECK(run_graph(tested, {{0}, literal::SKIP}, {0, 1}).equals({12, 28}));
}

} // namespace

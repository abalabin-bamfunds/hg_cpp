#include <hg/graph.hpp>
#include <hg/library.hpp>
#include <hg/map.hpp>
#include <hg/run_graph.hpp>
#include <hg/service.hpp>
#include <hg/service_catalogue.hpp>
#include <hg/time_series/tsd/tsd_type.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace hg;
using namespace hg::literals;

TEST_CASE("a request reply service can call another service",
          "[feature:service][series:tsd][series:value][shape:nested]") {
    const auto increment = service<auto(TS<i64>)->TS<i64>>{"increment", {"value"}};
    const auto add_ten = service<auto(TS<i64>)->TS<i64>>{"add_ten", {"value"}};

    const auto increment_value = node([](i64 value) { return value + 1; });
    const auto increment_impl = graph("name"_n = "increment_impl",
                                      [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) {
                                          return map(increment_value, values);
                                      });

    const auto add_ten_value = node([](i64 value) { return value + 10; });
    const auto call_increment = graph("name"_n = "call_increment", [&](wp<TS<i64>> value) {
        return add_ten_value(increment(value));
    });
    const auto add_ten_impl = graph("name"_n = "add_ten_impl",
                                    [&](wp<TSD<service_client_id_t, TS<i64>>, "value"_n> values) {
                                        return map(call_increment, values);
                                    });

    auto catalogue = ServiceCatalogue{};
    catalogue.register_impl(add_ten, add_ten_impl);
    catalogue.register_impl(increment, increment_impl);

    const auto tested = [&](wp<TS<i64>> value) { return add_ten(value); };

    CHECK(run_graph(tested, GraphRunOptions{.services = &catalogue}, {1, 2}).equals<i64>({12, 13}));
}

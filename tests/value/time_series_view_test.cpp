#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <type_traits>

using namespace bg;

static_assert(std::is_trivially_copyable_v<In<TS_T>>);

TEST_CASE("a time-series input reads validity and update time",
          "[time_series][feature:time-series-view][series:value][scalar:floating-point][transition:update]") {
    const auto value = 1.25F;
    const auto invalid = In<TS<float>>{};

    CHECK_FALSE(invalid.valid());

    const auto updated = MIN_DT + std::chrono::microseconds{1};
    const auto updated_state = TimeSeriesState{updated};
    const auto input = In<TS<float>>{updated_state, &value};

    CHECK(input.valid());
    CHECK(input.last_updated() == updated);
}

TEST_CASE("a set input preserves its packed validity state when type erased",
          "[time_series][feature:time-series-view][feature:visitor][declaration:generic][series:tss]") {
    auto state = TimeSeriesState{};
    const auto &runtime_type = cpp_time_series_runtime_type<TSS<i64>>();
    const auto input = In<TSS<i64>>{state, nullptr, runtime_type};
    const auto erased =
        In<TS_T>{input.last_updated_state(), input.data(), input.runtime_type(), input.modified()};

    CHECK_FALSE(erased.valid());

    const auto first_update = MIN_DT + std::chrono::microseconds{1};
    state.publish(first_update);
    CHECK(erased.valid());
    CHECK_FALSE(erased.became_valid());
    const auto updated = In<TS_T>{state, nullptr, runtime_type, true};

    const auto became_valid = updated.visit(
        [](const In<TSS<SCALAR>> *value) { return value->became_valid(); }, ddv::noop);

    REQUIRE(became_valid);
    CHECK(*became_valid);
    CHECK(erased.last_updated() == first_update);

    const auto invalidation_time = first_update + std::chrono::microseconds{1};
    state.invalidate(invalidation_time);
    CHECK_FALSE(erased.valid());
    const auto invalidated = In<TS_T>{state, nullptr, runtime_type, true};

    const auto became_invalid = invalidated.visit(
        [](const In<TSS<SCALAR>> *value) { return value->became_invalid(); }, ddv::noop);

    REQUIRE(became_invalid);
    CHECK(*became_invalid);
    CHECK(erased.last_updated() == invalidation_time);
}

namespace {

template <template <typename, auto...> typename Field> struct VisitorBundle {
    Field<TS<i64>> value;
};

} // namespace

TEST_CASE("time series input visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    const auto value = In<TS_T>{initial_time_series_state,
                                        nullptr,
                                        cpp_time_series_runtime_type<TS<i64>>()};
    const auto reference =
        In<TS_T>{initial_time_series_state,
                          nullptr,
                          cpp_time_series_runtime_type<REF<TS<i64>>>()};
    const auto list =
        In<TS_T>{initial_time_series_state,
                          nullptr,
                          cpp_time_series_runtime_type<TSL<TS<i64>, 2>>()};
    const auto set = In<TS_T>{initial_time_series_state,
                                      nullptr,
                                      cpp_time_series_runtime_type<TSS<i64>>()};
    const auto dict =
        In<TS_T>{initial_time_series_state,
                          nullptr,
                          cpp_time_series_runtime_type<TSD<i64, TS<i64>>>()};
    const auto bundle =
        In<TS_T>{initial_time_series_state,
                          nullptr,
                          cpp_time_series_runtime_type<TSB<VisitorBundle>>()};
    auto value_visited = false;
    auto reference_visited = false;
    auto list_visited = false;
    auto set_visited = false;
    auto dict_visited = false;
    auto bundle_visited = false;

    value.visit([&](const In<TS<SCALAR>> *) { value_visited = true; }, ddv::noop);
    reference.visit([&](const In<REF<TS_T>> *) { reference_visited = true; }, ddv::noop);
    list.visit([&](const In<TSL<TS_T, SIZE>> *) { list_visited = true; }, ddv::noop);
    set.visit([&](const In<TSS<SCALAR>> *) { set_visited = true; }, ddv::noop);
    dict.visit([&](const In<TSD<SCALAR, TS_T>> *) { dict_visited = true; }, ddv::noop);
    bundle.visit([&](const In<TSB<SCHEMA>> *) { bundle_visited = true; }, ddv::noop);

    CHECK(value_visited);
    CHECK(reference_visited);
    CHECK(list_visited);
    CHECK(set_visited);
    CHECK(dict_visited);
    CHECK(bundle_visited);
}

TEST_CASE("a time series visitor returns its result by value",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value]") {
    const auto updated = MIN_DT + std::chrono::microseconds{1};
    const auto updated_state = TimeSeriesState{updated};
    const auto input = In<TS_T>{updated_state,
                                        nullptr,
                                        cpp_time_series_runtime_type<TS<i64>>()};

    const auto result =
        input.visit([](const In<TS<SCALAR>> *value) { return value->last_updated(); }, ddv::noop);

    REQUIRE(result);
    CHECK(*result == updated);
}

TEST_CASE("time series output visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    auto value_state = details::OutputState{cpp_time_series_runtime_type<TS<i64>>()};
    auto reference_state = details::OutputState{cpp_time_series_runtime_type<REF<TS<i64>>>()};
    auto list_state = details::OutputState{cpp_time_series_runtime_type<TSL<TS<i64>, 2>>()};
    auto set_state = details::OutputState{cpp_time_series_runtime_type<TSS<i64>>()};
    auto dict_state = details::OutputState{cpp_time_series_runtime_type<TSD<i64, TS<i64>>>()};
    auto bundle_state = details::OutputState{cpp_time_series_runtime_type<TSB<VisitorBundle>>()};
    auto value = details::output_factory::make<Out<TS_T>>(value_state);
    auto reference = details::output_factory::make<Out<TS_T>>(reference_state);
    auto list = details::output_factory::make<Out<TS_T>>(list_state);
    auto set = details::output_factory::make<Out<TS_T>>(set_state);
    auto dict = details::output_factory::make<Out<TS_T>>(dict_state);
    auto bundle = details::output_factory::make<Out<TS_T>>(bundle_state);
    auto visited = 0;

    value.visit([&](const Out<TS<SCALAR>> *) { ++visited; }, ddv::noop);
    reference.visit([&](const Out<REF<TS_T>> *) { ++visited; }, ddv::noop);
    list.visit([&](const Out<TSL<TS_T, SIZE>> *) { ++visited; }, ddv::noop);
    set.visit([&](const Out<TSS<SCALAR>> *) { ++visited; }, ddv::noop);
    dict.visit([&](const Out<TSD<SCALAR, TS_T>> *) { ++visited; }, ddv::noop);
    bundle.visit([&](const Out<TSB<SCHEMA>> *) { ++visited; }, ddv::noop);

    CHECK(visited == 6);
}

TEST_CASE("a time series output visitor exposes the resolved output interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value]") {
    const auto evaluation_time = MIN_DT + std::chrono::microseconds{1};
    auto last_updated = TimeSeriesState{};
    auto stored = i64{};
    auto state = details::OutputState{cpp_time_series_runtime_type<TS<i64>>()};
    state.bind(last_updated, &stored, evaluation_time);
    auto output = details::output_factory::make<Out<TS_T>>(state);
    const auto value = ScalarValue<i64>{42};
    auto called = false;

    output.visit(
        [&](const Out<TS<SCALAR>> *resolved) {
            called = true;
            resolved->set_value(value.view());
        },
        ddv::noop);

    CHECK(called);
    CHECK(stored == 42);
    CHECK(last_updated.time() == evaluation_time);
}

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

static_assert(std::is_trivially_copyable_v<bg::In<bg::TS_T>>);

TEST_CASE("a time-series input reads validity and update time",
          "[time_series][feature:time-series-view][series:value][scalar:floating-point][transition:update]") {
    const auto value = 1.25F;
    const auto invalid = bg::In<bg::TS<float>>{};

    CHECK_FALSE(invalid.valid());

    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto updated_state = bg::TimeSeriesState{updated};
    const auto input = bg::In<bg::TS<float>>{updated_state, &value};

    CHECK(input.valid());
    CHECK(input.last_updated() == updated);
}

TEST_CASE("a set input preserves its packed validity state when type erased",
          "[time_series][feature:time-series-view][feature:visitor][declaration:generic][series:tss]") {
    auto state = bg::TimeSeriesState{};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::TSS<bg::i64>>();
    const auto input = bg::In<bg::TSS<bg::i64>>{state, nullptr, runtime_type};
    const auto erased =
        bg::In<bg::TS_T>{input.last_updated_state(), input.data(), input.runtime_type(), input.modified()};

    CHECK_FALSE(erased.valid());

    const auto first_update = bg::MIN_DT + std::chrono::microseconds{1};
    state.publish(first_update);
    CHECK(erased.valid());

    const auto became_valid = erased.visit(
        [](const bg::In<bg::TSS<bg::SCALAR>> *value) { return value->became_valid(); }, ddv::noop);

    REQUIRE(became_valid);
    CHECK(*became_valid);
    CHECK(erased.last_updated() == first_update);

    const auto invalidation_time = first_update + std::chrono::microseconds{1};
    state.invalidate(invalidation_time);
    CHECK_FALSE(erased.valid());

    const auto became_invalid = erased.visit(
        [](const bg::In<bg::TSS<bg::SCALAR>> *value) { return value->became_invalid(); }, ddv::noop);

    REQUIRE(became_invalid);
    CHECK(*became_invalid);
    CHECK(erased.last_updated() == invalidation_time);
}

namespace {

template <template <typename, auto...> typename Field> struct VisitorBundle {
    Field<bg::TS<bg::i64>> value;
};

} // namespace

TEST_CASE("time series input visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    const auto value = bg::In<bg::TS_T>{bg::initial_time_series_state,
                                        nullptr,
                                        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};
    const auto reference =
        bg::In<bg::TS_T>{bg::initial_time_series_state,
                          nullptr,
                          bg::cpp_time_series_runtime_type<bg::REF<bg::TS<bg::i64>>>()};
    const auto list =
        bg::In<bg::TS_T>{bg::initial_time_series_state,
                          nullptr,
                          bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 2>>()};
    const auto set = bg::In<bg::TS_T>{bg::initial_time_series_state,
                                      nullptr,
                                      bg::cpp_time_series_runtime_type<bg::TSS<bg::i64>>()};
    const auto dict =
        bg::In<bg::TS_T>{bg::initial_time_series_state,
                          nullptr,
                          bg::cpp_time_series_runtime_type<bg::TSD<bg::i64, bg::TS<bg::i64>>>()};
    const auto bundle =
        bg::In<bg::TS_T>{bg::initial_time_series_state,
                          nullptr,
                          bg::cpp_time_series_runtime_type<bg::TSB<VisitorBundle>>()};
    auto value_visited = false;
    auto reference_visited = false;
    auto list_visited = false;
    auto set_visited = false;
    auto dict_visited = false;
    auto bundle_visited = false;

    value.visit([&](const bg::In<bg::TS<bg::SCALAR>> *) { value_visited = true; }, ddv::noop);
    reference.visit([&](const bg::In<bg::REF<bg::TS_T>> *) { reference_visited = true; }, ddv::noop);
    list.visit([&](const bg::In<bg::TSL<bg::TS_T, bg::SIZE>> *) { list_visited = true; }, ddv::noop);
    set.visit([&](const bg::In<bg::TSS<bg::SCALAR>> *) { set_visited = true; }, ddv::noop);
    dict.visit([&](const bg::In<bg::TSD<bg::SCALAR, bg::TS_T>> *) { dict_visited = true; }, ddv::noop);
    bundle.visit([&](const bg::In<bg::TSB<bg::SCHEMA>> *) { bundle_visited = true; }, ddv::noop);

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
    const auto updated = bg::MIN_DT + std::chrono::microseconds{1};
    const auto updated_state = bg::TimeSeriesState{updated};
    const auto input = bg::In<bg::TS_T>{updated_state,
                                        nullptr,
                                        bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};

    const auto result =
        input.visit([](const bg::In<bg::TS<bg::SCALAR>> *value) { return value->last_updated(); }, ddv::noop);

    REQUIRE(result);
    CHECK(*result == updated);
}

TEST_CASE("time series output visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    auto value_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};
    auto reference_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::REF<bg::TS<bg::i64>>>()};
    auto list_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<bg::i64>, 2>>()};
    auto set_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSS<bg::i64>>()};
    auto dict_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSD<bg::i64, bg::TS<bg::i64>>>()};
    auto bundle_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSB<VisitorBundle>>()};
    auto value = bg::details::output_factory::make<bg::Out<bg::TS_T>>(value_state);
    auto reference = bg::details::output_factory::make<bg::Out<bg::TS_T>>(reference_state);
    auto list = bg::details::output_factory::make<bg::Out<bg::TS_T>>(list_state);
    auto set = bg::details::output_factory::make<bg::Out<bg::TS_T>>(set_state);
    auto dict = bg::details::output_factory::make<bg::Out<bg::TS_T>>(dict_state);
    auto bundle = bg::details::output_factory::make<bg::Out<bg::TS_T>>(bundle_state);
    auto visited = 0;

    value.visit([&](const bg::Out<bg::TS<bg::SCALAR>> *) { ++visited; }, ddv::noop);
    reference.visit([&](const bg::Out<bg::REF<bg::TS_T>> *) { ++visited; }, ddv::noop);
    list.visit([&](const bg::Out<bg::TSL<bg::TS_T, bg::SIZE>> *) { ++visited; }, ddv::noop);
    set.visit([&](const bg::Out<bg::TSS<bg::SCALAR>> *) { ++visited; }, ddv::noop);
    dict.visit([&](const bg::Out<bg::TSD<bg::SCALAR, bg::TS_T>> *) { ++visited; }, ddv::noop);
    bundle.visit([&](const bg::Out<bg::TSB<bg::SCHEMA>> *) { ++visited; }, ddv::noop);

    CHECK(visited == 6);
}

TEST_CASE("a time series output visitor exposes the resolved output interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value]") {
    const auto evaluation_time = bg::MIN_DT + std::chrono::microseconds{1};
    auto last_updated = bg::TimeSeriesState{};
    auto stored = bg::i64{};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<bg::i64>>()};
    state.bind(last_updated, &stored, evaluation_time);
    auto output = bg::details::output_factory::make<bg::Out<bg::TS_T>>(state);
    const auto value = bg::ScalarValue<bg::i64>{42};
    auto called = false;

    output.visit(
        [&](const bg::Out<bg::TS<bg::SCALAR>> *resolved) {
            called = true;
            resolved->set_value(value.view());
        },
        ddv::noop);

    CHECK(called);
    CHECK(stored == 42);
    CHECK(last_updated.time() == evaluation_time);
}

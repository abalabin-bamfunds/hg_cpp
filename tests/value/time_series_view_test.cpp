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
    const auto input = bg::In<bg::TS<float>>{updated, &value};

    CHECK(input.valid());
    CHECK(input.last_updated() == updated);
}

namespace {

template <template <typename, auto...> typename Field> struct VisitorBundle {
    Field<bg::TS<int>> value;
};

} // namespace

TEST_CASE("time series input visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    const auto value = bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::TS<std::int64_t>>()};
    const auto reference =
        bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::REF<bg::TS<std::int64_t>>>()};
    const auto list =
        bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<std::int64_t>, 2>>()};
    const auto set = bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::TSS<std::int64_t>>()};
    const auto dict =
        bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::TSD<std::int64_t, bg::TS<int>>>()};
    const auto bundle =
        bg::In<bg::TS_T>{bg::MIN_DT, nullptr, bg::cpp_time_series_runtime_type<bg::TSB<VisitorBundle>>()};
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
    const auto input = bg::In<bg::TS_T>{updated, nullptr, bg::cpp_time_series_runtime_type<bg::TS<std::int64_t>>()};

    const auto result =
        input.visit([](const bg::In<bg::TS<bg::SCALAR>> *value) { return value->last_updated(); }, ddv::noop);

    REQUIRE(result);
    CHECK(*result == updated);
}

TEST_CASE("time series output visitors select every resolved interface",
          "[time_series][visitor][feature:time-series-view][feature:visitor][declaration:generic]"
          "[series:generic][series:value][series:reference][series:fixed-tsl][series:tss][series:tsd][series:tsb]") {
    auto value_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<std::int64_t>>()};
    auto reference_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::REF<bg::TS<std::int64_t>>>()};
    auto list_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSL<bg::TS<std::int64_t>, 2>>()};
    auto set_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSS<std::int64_t>>()};
    auto dict_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSD<std::int64_t, bg::TS<int>>>()};
    auto bundle_state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TSB<VisitorBundle>>()};
    auto value = bg::details::output_factory::make<bg::TS_T>(value_state);
    auto reference = bg::details::output_factory::make<bg::TS_T>(reference_state);
    auto list = bg::details::output_factory::make<bg::TS_T>(list_state);
    auto set = bg::details::output_factory::make<bg::TS_T>(set_state);
    auto dict = bg::details::output_factory::make<bg::TS_T>(dict_state);
    auto bundle = bg::details::output_factory::make<bg::TS_T>(bundle_state);
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
    auto last_updated = bg::MIN_DT;
    auto stored = std::int64_t{};
    auto state = bg::details::OutputState{bg::cpp_time_series_runtime_type<bg::TS<std::int64_t>>()};
    state.bind(last_updated, &stored, evaluation_time);
    auto output = bg::details::output_factory::make<bg::TS_T>(state);
    const auto value = bg::ScalarValue<std::int64_t>{42};
    auto called = false;

    output.visit(
        [&](const bg::Out<bg::TS<bg::SCALAR>> *resolved) {
            called = true;
            resolved->set_value(value.view());
        },
        ddv::noop);

    CHECK(called);
    CHECK(stored == 42);
    CHECK(last_updated == evaluation_time);
}

#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

TEST_CASE("a time series reference value is one pointer",
          "[value][time-series][reference][feature:time-series-storage]") {
    STATIC_CHECK(sizeof(bg::details::ReferenceAddress) == sizeof(void *));
    STATIC_CHECK(sizeof(bg::details::ReferenceRoot) == 4 * sizeof(void *));

    const auto &reference_type = bg::REF<bg::TS<int>>::type();
    const auto &runtime_type = bg::time_series_reference_runtime_type(reference_type);

    CHECK(runtime_type.value_size() == sizeof(bg::details::ReferenceAddress));
    CHECK(runtime_type.value_alignment() == alignof(bg::details::ReferenceAddress));
}

TEST_CASE("a time series reference propagates its storage pointer",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto target = bg::details::ReferenceRoute{};
    auto source_value = bg::details::ReferenceAddress{&target};
    auto output_value = bg::details::ReferenceAddress{};
    auto source_time = bg::EvaluationTime{bg::EvaluationTime::duration{1}};
    auto output_time = bg::MIN_DT;
    const auto evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{2}};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::REF<bg::TS<int>>>();

    const auto source = bg::In<bg::REF<bg::TS<int>>>{source_time, &source_value};
    auto state = bg::details::OutputState{runtime_type};
    state.bind(output_time, &output_value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::REF<bg::TS<int>>>(state);
    output.set(source);

    CHECK(output_value == source_value);
    CHECK(output_time == evaluation_time);
}

TEST_CASE("a reference item route retains its parent and item",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto root = bg::details::ReferenceRoot{};
    auto outer = bg::details::ReferenceRoute{};
    auto inner = bg::details::ReferenceRoute{};

    outer.bind_item(bg::details::ReferenceAddress{&root}, 12);
    inner.bind_item(bg::details::ReferenceAddress{&outer}, 34);

    CHECK(inner.parent.get<bg::details::ReferenceRoute>() == &outer);
    CHECK(inner.item() == 34);
    CHECK(outer.parent.get<bg::details::ReferenceRoot>() == &root);
    CHECK(outer.item() == 12);
}

TEST_CASE("a reference representation route retains its source and representation index",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto root = bg::details::ReferenceRoot{};
    auto source = bg::details::ReferenceRoute{};
    auto representation = bg::details::ReferenceRoute{};

    source.bind_item(bg::details::ReferenceAddress{&root}, 12);
    representation.bind_representation(bg::details::ReferenceAddress{&source}, 7);

    CHECK(representation.parent.get<bg::details::ReferenceRoute>() == &source);
    CHECK(representation.is_representation());
    CHECK(representation.representation() == 7);
    STATIC_CHECK(sizeof(bg::details::ReferenceRoute) == 3 * sizeof(void *));
}

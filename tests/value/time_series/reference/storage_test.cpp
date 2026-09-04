#include <bg/node/input.hpp>
#include <bg/node/output.hpp>
#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a time series reference value is one pointer",
          "[value][time-series][reference][feature:time-series-storage]") {
    const auto &target_type = bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &target_runtime_type =
        bg::time_series_value_runtime_type(target_type, bg::cpp_scalar_runtime_type<int>());
    const auto &reference_type = bg::time_series_reference_type(target_type, bg::TimeSeriesGuarantee::ordinary);
    const auto &runtime_type = bg::time_series_reference_runtime_type(reference_type, target_runtime_type);

    CHECK(runtime_type.value_size() == sizeof(bg::details::ReferenceStorage *));
    CHECK(runtime_type.value_alignment() == alignof(bg::details::ReferenceStorage *));
}

TEST_CASE("a time series reference propagates its storage pointer",
          "[value][time-series][reference][feature:time-series-storage]") {
    auto target = bg::details::ReferenceStorage{};
    auto *source_value = &target;
    auto *output_value = static_cast<bg::details::ReferenceStorage *>(nullptr);
    auto source_time = bg::EvaluationTime{bg::EvaluationTime::duration{1}};
    auto output_time = bg::MIN_DT;
    const auto evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{2}};
    const auto &runtime_type = bg::cpp_time_series_runtime_type<bg::REF<bg::TS<int>>>();

    const auto source = bg::In<bg::REF<bg::TS<int>>>{source_time, &source_value};
    auto state = bg::details::OutputState{runtime_type};
    state.bind(output_time, &output_value, evaluation_time);
    const auto output = bg::details::output_factory::make<bg::REF<bg::TS<int>>>(state);
    output.set(source);

    CHECK(output_value == &target);
    CHECK(output_time == evaluation_time);
}

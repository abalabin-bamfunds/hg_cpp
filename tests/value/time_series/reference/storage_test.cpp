#include <bg/scalar/cpp_storage_representation.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("a time series reference value is one pointer", "[value][time-series][reference]") {
    const auto &target_type =
        bg::time_series_value_type(bg::cpp_scalar_type<int>(), bg::TimeSeriesGuarantee::ordinary);
    const auto &target_representation =
        bg::time_series_value_storage_representation(target_type, bg::cpp_scalar_storage_representation<int>());
    const auto &reference_type = bg::time_series_reference_type(target_type, bg::TimeSeriesGuarantee::ordinary);
    const auto &representation =
        bg::time_series_reference_storage_representation(reference_type, target_representation);

    CHECK(representation.value_size() == sizeof(bg::details::ReferenceStorage *));
    CHECK(representation.value_alignment() == alignof(bg::details::ReferenceStorage *));
}

TEST_CASE("a time series reference propagates its storage pointer", "[value][time-series][reference]") {
    auto target = bg::details::ReferenceStorage{};
    auto *source_value = &target;
    auto *output_value = static_cast<bg::details::ReferenceStorage *>(nullptr);
    auto source_time = bg::EvaluationTime{bg::EvaluationTime::duration{1}};
    auto output_time = bg::MIN_DT;
    const auto evaluation_time = bg::EvaluationTime{bg::EvaluationTime::duration{2}};

    auto source = bg::TimeSeriesReferenceInputView{};
    source.bind(source_time, &source_value);
    auto output = bg::TimeSeriesReferenceOutputView{};
    output.bind(output_time, &output_value, evaluation_time);
    output.set_reference(source);

    CHECK(output_value == &target);
    CHECK(output_time == evaluation_time);
}

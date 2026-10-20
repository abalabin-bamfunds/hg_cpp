#pragma once

#include <bg/frame/export.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/literal_operations.hpp>
#include <bg/time_series/literal_operations.hpp>
#include <bg/time_series/output_state.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>

namespace bg::frame {

extern BG_FRAME_EXPORT const literal::Sentinel NULL_VALUE;

namespace details {
BG_FRAME_EXPORT void register_column_literal_type(const ScalarType &type);
BG_FRAME_EXPORT void register_frame_literal_type(const ScalarType &type);
BG_FRAME_EXPORT void register_literal_runtime(const ScalarRuntimeType &runtime,
                                              ScalarLiteralOperations operations);
BG_FRAME_EXPORT void apply_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output);
BG_FRAME_EXPORT void register_frame_series_literal_runtime(const TimeSeriesRuntimeType &runtime);
BG_FRAME_EXPORT void register_column_series_literal_runtime(const TimeSeriesRuntimeType &runtime);
BG_FRAME_EXPORT void register_frame_series_tick_comparison(const TimeSeriesType &type);
BG_FRAME_EXPORT void register_frame_series_tick_writer(const TimeSeriesRuntimeType &runtime);
} // namespace details

} // namespace bg::frame

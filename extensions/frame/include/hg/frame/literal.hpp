#pragma once

#include <hg/frame/export.hpp>
#include <hg/literal/sentinel.hpp>
#include <hg/scalar/literal.hpp>
#include <hg/scalar/literal_operations.hpp>
#include <hg/time_series/literal_operations.hpp>
#include <hg/time_series/output_state.hpp>
#include <hg/time_series/time_series_runtime_type.hpp>

namespace hg::frame {

extern HG_FRAME_EXPORT const literal::Sentinel NULL_VALUE;

namespace details {
HG_FRAME_EXPORT void register_column_literal_type(const ScalarType &type);
HG_FRAME_EXPORT void register_frame_literal_type(const ScalarType &type);
HG_FRAME_EXPORT void register_literal_runtime(const ScalarRuntimeType &runtime,
                                              ScalarLiteralOperations operations);
HG_FRAME_EXPORT void apply_series_literal(const literal::TimeSeriesLiteral &literal, OutputState &output);
HG_FRAME_EXPORT void register_frame_series_literal_runtime(const TimeSeriesRuntimeType &runtime);
HG_FRAME_EXPORT void register_column_series_literal_runtime(const TimeSeriesRuntimeType &runtime);
HG_FRAME_EXPORT void register_frame_series_tick_comparison(const TimeSeriesType &type);
HG_FRAME_EXPORT void register_frame_series_tick_replay(const TimeSeriesRuntimeType &runtime);
template <typename T>
HG_FRAME_EXPORT void register_column_series_tick_writer(const TimeSeriesRuntimeType &runtime);
template <typename T>
HG_FRAME_EXPORT void register_column_series_tick_replay(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_writer<i64>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_writer<double>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_replay<bool>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_replay<i64>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_replay<double>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_replay<string>(const TimeSeriesRuntimeType &runtime);
extern template HG_FRAME_EXPORT void register_column_series_tick_replay<symbol>(const TimeSeriesRuntimeType &runtime);
} // namespace details

} // namespace hg::frame

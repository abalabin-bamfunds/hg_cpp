#pragma once

#include <bg/frame/export.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/scalar/literal.hpp>
#include <bg/scalar/literal_operations.hpp>

namespace bg::frame {

extern BG_FRAME_EXPORT const literal::Sentinel NULL_VALUE;

namespace details {
BG_FRAME_EXPORT void register_column_literal_type(const ScalarType &type);
BG_FRAME_EXPORT void register_frame_literal_type(const ScalarType &type);
BG_FRAME_EXPORT void register_literal_runtime(const ScalarRuntimeType &runtime,
                                              ScalarLiteralOperations operations);
} // namespace details

} // namespace bg::frame

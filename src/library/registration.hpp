#pragma once

namespace bg::operators::details {

void register_numeric_operators();
void register_conversion_operators();
void register_collect_operators();
void register_emit_operators();
void register_string_operators();
void register_string_regex_operators();
void register_string_collection_operators();
void register_string_format_operators();
void register_logical_operators();
void register_stream_operators();
void register_analytical_operators();
void register_running_operators();
void register_timed_operators();
void register_collection_operators();
void register_collection_reduction_operators();
void register_tss_operators();

} // namespace bg::operators::details

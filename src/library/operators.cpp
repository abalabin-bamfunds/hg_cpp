#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>

#include <limits>

namespace bg::operators {
using namespace bg::literals;

decltype(add) add{"add", {"lhs", "rhs"}};
decltype(sub) sub{"sub", {"lhs", "rhs"}};
decltype(mul) mul{"mul", {"lhs", "rhs"}};
decltype(div) div{"div", {"lhs", "rhs"}};
decltype(floordiv) floordiv{"floordiv", {"lhs", "rhs"}};
decltype(mod) mod{"mod", {"lhs", "rhs"}};
decltype(pow) pow{"pow", {"lhs", "rhs"}};
decltype(round) round{"round", {"value", "ndigits"}};
decltype(pos) pos{"pos", {"value"}};
decltype(neg) neg{"neg", {"value"}};
decltype(abs) abs{"abs", {"value"}};
decltype(invert) invert{"invert", {"value"}};

decltype(convert) convert{"convert", {"value"}};
decltype(combine) combine{"combine", {"values"}};
decltype(collect) collect{"collect", {"value", "reset"}};
decltype(emit) emit{"emit", {"value"}};

// Text.
decltype(substr) substr{"substr", {"value", "start", "end"}};
decltype(lower) lower{"lower", {"value"}};
decltype(upper) upper{"upper", {"value"}};
decltype(casefold) casefold{"casefold", {"value"}};
decltype(str_) str_{"str_", {"value"}};
decltype(format_) format_{"format_", {"pattern"}};
decltype(print_) print_{"print_", {"pattern"}};
decltype(log_) log_{"log_", {"pattern"}};
decltype(matches) matches{"matches", {"pattern", "text"}};
decltype(replace) replace{"replace", {"pattern", "replacement", "text"}};
decltype(match_) match_{"match_", {"pattern", "text"}};
decltype(split) split{"split", {"text", "separator", "maxsplit"}};
decltype(join) join{"join", {"values", "separator"}};

decltype(eq) eq{"eq", {"lhs", "rhs"}};
decltype(ne) ne{"ne", {"lhs", "rhs"}};
decltype(lt) lt{"lt", {"lhs", "rhs"}};
decltype(le) le{"le", {"lhs", "rhs"}};
decltype(gt) gt{"gt", {"lhs", "rhs"}};
decltype(ge) ge{"ge", {"lhs", "rhs"}};

decltype(and_) and_{"and_", {"lhs", "rhs"}};
decltype(or_) or_{"or_", {"lhs", "rhs"}};
decltype(not_) not_{"not_", {"value"}};

decltype(valid) valid{"valid", {"value"}};
decltype(all_valid) all_valid{"all_valid", {"value"}};
decltype(type_) type_{"type_", {"value"}};
decltype(count) count{"count", {"value"}};

decltype(sample) sample{"sample", {"signal", "value"}};
decltype(dedup) dedup{"dedup", {"value"}};
decltype(if_) if_{"if_", {"condition", "value"}};
decltype(filter_) filter_{"filter_", {"condition", "value"}};
decltype(gate) gate{"gate", {"condition", "value", "buffer_length"_n = std::numeric_limits<i64>::max(), "reset"}};
decltype(batch)
    batch{"batch", {"condition", "value", "delay", "buffer_length"_n = std::numeric_limits<i64>::max()}};
decltype(merge) merge{"merge", {"values"}};
decltype(race) race{"race", {"values"}};
decltype(route_by_index) route_by_index{"route_by_index", {"index", "value"}};
decltype(if_true) if_true{"if_true", {"condition", "tick_once_only"_n = false}};
decltype(take) take{"take", {"value", "n"}};
decltype(drop) drop{"drop", {"value", "n"}};
decltype(step) step{"step", {"value", "n"}};
decltype(slice_) slice_{"slice_", {"value", "start", "stop", "step"_n = i64{1}}};
decltype(if_then_else) if_then_else{"if_then_else", {"condition", "true_value", "false_value"}};
decltype(lag) lag{"lag", {"value", "n"_n = i64{1}}};
decltype(last_modified_time) last_modified_time{"last_modified_time", {"value"}};
decltype(elapsed) elapsed{"elapsed", {"value"}};
decltype(diff) diff{"diff", {"value"}};
decltype(clip) clip{"clip", {"value", "min_", "max_"}};
decltype(ewma) ewma{"ewma", {"value", "alpha", "min_periods"_n = i64{0}}};
decltype(delay) delay{"delay", {"value", "duration"}};
decltype(schedule) schedule{"schedule", {"period", "initial_delay", "max_ticks"}};
decltype(throttle) throttle{"throttle", {"value", "period", "delay_first_tick"}};
decltype(debounce) debounce{"debounce", {"value", "period"}};
decltype(timeout) timeout{"timeout", {"value", "period"}};
decltype(resample) resample{"resample", {"value", "period"}};
decltype(sums) sums{"sums", {"value"}};
decltype(mins) mins{"mins", {"value"}};
decltype(maxs) maxs{"maxs", {"value"}};
decltype(means) means{"means", {"value"}};

decltype(len_) len_{"len_", {"value"}};
decltype(contains_) contains_{"contains_", {"collection", "key"}};
decltype(is_empty) is_empty{"is_empty", {"value"}};
decltype(keys_) keys_{"keys_", {"dictionary"}};
decltype(values_) values_{"values_", {"dictionary"}};
decltype(index_of) index_of{"index_of", {"values", "item"}};
decltype(keys_where_true) keys_where_true{"keys_where_true", {"mask"}};
decltype(bit_or) bit_or{"bit_or", {"lhs", "rhs"}};
decltype(bit_and) bit_and{"bit_and", {"lhs", "rhs"}};
decltype(bit_xor) bit_xor{"bit_xor", {"lhs", "rhs"}};
decltype(rekey) rekey{"rekey", {"ts", "new_keys"}};
decltype(partition) partition{"partition", {"ts", "partitions", "remove_empty"_n = true}};
decltype(unpartition) unpartition{"unpartition", {"ts"}};
decltype(flip) flip{"flip", {"ts", "unique"_n = true}};
decltype(flip_keys) flip_keys{"flip_keys", {"ts", "remove_empty"_n = true}};
decltype(collapse_keys) collapse_keys{"collapse_keys", {"ts"}};
decltype(uncollapse_keys) uncollapse_keys{"uncollapse_keys", {"ts", "remove_empty"_n = true}};
decltype(sum_) sum_{"sum_", {"values"}};
decltype(min_) min_{"min_", {"values"}};
decltype(max_) max_{"max_", {"values"}};
decltype(mean) mean{"mean", {"values"}};
decltype(all_) all_{"all_", {"values"}};
decltype(any_) any_{"any_", {"values"}};

namespace {

// Register implementations only after every operator object above has been constructed.
const auto registered = [] {
    details::register_numeric_operators();
    details::register_conversion_operators();
    details::register_collect_operators();
    details::register_emit_operators();
    details::register_string_operators();
    details::register_string_regex_operators();
    details::register_string_collection_operators();
    details::register_string_format_operators();
    details::register_log_operators();
    details::register_logical_operators();
    details::register_stream_operators();
    details::register_buffering_operators();
    details::register_merge_operators();
    details::register_analytical_operators();
    details::register_running_operators();
    details::register_timed_operators();
    details::register_collection_operators();
    details::register_collection_reduction_operators();
    details::register_tss_operators();
    details::register_tsd_transform_operators();
    return true;
}();

} // namespace
} // namespace bg::operators

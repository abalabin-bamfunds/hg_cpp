#include "registration.hpp"

#include <bg/library/operators.hpp>

namespace bg::operators {

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
decltype(filter_) filter_{"filter_", {"condition", "value"}};
decltype(if_then_else) if_then_else{"if_then_else", {"condition", "true_value", "false_value"}};
decltype(lag) lag{"lag", {"value"}};
decltype(delay) delay{"delay", {"value", "duration"}};
decltype(schedule) schedule{"schedule", {"period", "initial_delay", "max_ticks"}};
decltype(throttle) throttle{"throttle", {"value", "period", "delay_first_tick"}};
decltype(sums) sums{"sums", {"value"}};
decltype(mins) mins{"mins", {"value"}};
decltype(maxs) maxs{"maxs", {"value"}};
decltype(means) means{"means", {"value"}};

decltype(len_) len_{"len_", {"value"}};
decltype(contains_) contains_{"contains_", {"collection", "key"}};
decltype(is_empty) is_empty{"is_empty", {"value"}};
decltype(bit_or) bit_or{"bit_or", {"lhs", "rhs"}};
decltype(bit_and) bit_and{"bit_and", {"lhs", "rhs"}};
decltype(bit_xor) bit_xor{"bit_xor", {"lhs", "rhs"}};
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
    details::register_string_operators();
    details::register_string_regex_operators();
    details::register_string_collection_operators();
    details::register_string_format_operators();
    details::register_logical_operators();
    details::register_stream_operators();
    details::register_running_operators();
    details::register_timed_operators();
    details::register_collection_operators();
    details::register_collection_reduction_operators();
    details::register_tss_operators();
    return true;
}();

} // namespace
} // namespace bg::operators

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

decltype(sample) sample{"sample", {"signal", "value"}};
decltype(dedup) dedup{"dedup", {"value"}};
decltype(filter_) filter_{"filter_", {"condition", "value"}};
decltype(if_then_else) if_then_else{"if_then_else", {"condition", "true_value", "false_value"}};

namespace {

// Register implementations only after every operator object above has been constructed.
const auto registered = [] {
    details::register_numeric_operators();
    details::register_string_operators();
    details::register_logical_operators();
    details::register_stream_operators();
    return true;
}();

} // namespace
} // namespace bg::operators

#include "registration.hpp"

#include <bg/library/operators.hpp>

namespace bg::operators {

decltype(add) add{"add", {"lhs", "rhs"}};
decltype(sub) sub{"sub", {"lhs", "rhs"}};
decltype(mul) mul{"mul", {"lhs", "rhs"}};
decltype(pos) pos{"pos", {"value"}};
decltype(neg) neg{"neg", {"value"}};
decltype(abs) abs{"abs", {"value"}};
decltype(eq) eq{"eq", {"lhs", "rhs"}};
decltype(ne) ne{"ne", {"lhs", "rhs"}};
decltype(lt) lt{"lt", {"lhs", "rhs"}};
decltype(le) le{"le", {"lhs", "rhs"}};
decltype(gt) gt{"gt", {"lhs", "rhs"}};
decltype(ge) ge{"ge", {"lhs", "rhs"}};
decltype(and_) and_{"and_", {"lhs", "rhs"}};
decltype(or_) or_{"or_", {"lhs", "rhs"}};
decltype(not_) not_{"not_", {"value"}};

namespace {

// Register implementations only after every operator object above has been constructed.
const auto registered = [] {
    details::register_numeric_operators();
    details::register_string_operators();
    details::register_logical_operators();
    return true;
}();

} // namespace
} // namespace bg::operators

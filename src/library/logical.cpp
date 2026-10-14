#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

namespace bg::operators::details {

namespace {

struct logical_overloads {
    // Equality.
    overload<eq> equal_bool{[](bool lhs, bool rhs) { return lhs == rhs; }};
    overload<eq> equal_byte{[](byte lhs, byte rhs) { return lhs == rhs; }};
    overload<eq> equal_time_series{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return lhs.value_view().equals(rhs.value_view());
    }};

    // Inequality.
    overload<ne> not_equal_bool{[](bool lhs, bool rhs) { return lhs != rhs; }};
    overload<ne> not_equal_byte{[](byte lhs, byte rhs) { return lhs != rhs; }};
    overload<ne> not_equal_time_series{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return !lhs.value_view().equals(rhs.value_view());
    }};

    // Boolean logic.
    overload<and_> logical_and{[](bool lhs, bool rhs) { return lhs && rhs; }};
    overload<or_> logical_or{[](bool lhs, bool rhs) { return lhs || rhs; }};
    overload<not_> logical_not{[](bool value) { return !value; }};
};

} // namespace

void register_logical_operators() {
    (void)logical_overloads{};
}

} // namespace bg::operators::details

#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>

namespace bg::operators::details {

namespace {

struct logical_overloads {
    // Equality.
    overload<eq> eq_bool{[](bool lhs, bool rhs) { return lhs == rhs; }};
    overload<eq> eq_byte{[](byte lhs, byte rhs) { return lhs == rhs; }};
    overload<eq> eq_impl{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return lhs.value_view().equals(rhs.value_view());
    }};

    // Inequality.
    overload<ne> ne_bool{[](bool lhs, bool rhs) { return lhs != rhs; }};
    overload<ne> ne_byte{[](byte lhs, byte rhs) { return lhs != rhs; }};
    overload<ne> ne_impl{[](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return !lhs.value_view().equals(rhs.value_view());
    }};

    // Boolean logic.
    overload<and_> and_impl{[](bool lhs, bool rhs) { return lhs && rhs; }};
    overload<or_> or_impl{[](bool lhs, bool rhs) { return lhs || rhs; }};
    overload<not_> not_impl{[](bool value) { return !value; }};
};

} // namespace

void register_logical_operators() {
    register_overloads<logical_overloads>();
}

} // namespace bg::operators::details

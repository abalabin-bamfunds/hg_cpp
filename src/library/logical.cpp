#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>

namespace bg::operators::details {

void register_logical_operators() {
    using EQUALITY_SCALAR = operators::EQUALITY_SCALAR;
    eq.overload(node([](bool lhs, bool rhs) { return lhs == rhs; }));
    eq.overload(node([](byte lhs, byte rhs) { return lhs == rhs; }));
    eq.overload(node([](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return lhs.value_view().equals(rhs.value_view());
    }));

    ne.overload(node([](bool lhs, bool rhs) { return lhs != rhs; }));
    ne.overload(node([](byte lhs, byte rhs) { return lhs != rhs; }));
    ne.overload(node([](In<TS<EQUALITY_SCALAR>> lhs, In<TS<EQUALITY_SCALAR>> rhs) {
        return !lhs.value_view().equals(rhs.value_view());
    }));

    and_.overload(node([](bool lhs, bool rhs) { return lhs && rhs; }));
    or_.overload(node([](bool lhs, bool rhs) { return lhs || rhs; }));
    not_.overload(node([](bool value) { return !value; }));
}

} // namespace bg::operators::details

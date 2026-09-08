#pragma once

namespace bg::details {

template <typename Declaration> struct scalar_type_access {
    static decltype(auto) type();
};

} // namespace bg::details

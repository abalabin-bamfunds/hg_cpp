#pragma once

namespace hg::details {

template <typename Declaration> struct scalar_type_access {
    static decltype(auto) type();
};

} // namespace hg::details

#pragma once

#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Declaration> struct declaration_variable_traits {
    template <typename Visitor> static void for_each(Visitor &&) {}
};

template <typename Declaration, typename Visitor> void for_each_declaration_variable(Visitor &&visitor) {
    declaration_variable_traits<std::remove_cv_t<Declaration>>::for_each(std::forward<Visitor>(visitor));
}

} // namespace bg::details

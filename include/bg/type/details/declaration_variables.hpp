#pragma once

#include <type_traits>
#include <utility>

namespace bg::details {

template <typename Declaration> struct declaration_variable_traits {
    template <typename Visitor> static void for_each(Visitor &&) {}
};

} // namespace bg::details

namespace bg {

// Declares the variables contributed by a C++ declaration. Providers may specialize this trait for their declarations.
template <typename Declaration>
struct declaration_variable_traits : details::declaration_variable_traits<std::remove_cv_t<Declaration>> {};

template <typename Declaration, typename Visitor> void for_each_declaration_variable(Visitor &&visitor) {
    declaration_variable_traits<std::remove_cv_t<Declaration>>::for_each(std::forward<Visitor>(visitor));
}

} // namespace bg

namespace bg::details {

template <typename Declaration, typename Visitor> void for_each_declaration_variable(Visitor &&visitor) {
    ::bg::for_each_declaration_variable<Declaration>(std::forward<Visitor>(visitor));
}

} // namespace bg::details

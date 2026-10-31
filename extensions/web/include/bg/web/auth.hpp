#pragma once

#include <bg/scalar/bundle/value.hpp>
#include <bg/optionality.hpp>
#include <bg/util/named_argument.hpp>
#include <bg/web/types.hpp>

namespace bg::web::details {

template <typename IDENTITY> struct AuthDecisionSchema {
    template <template <typename, auto...> typename Field> struct schema {
        Field<IDENTITY, optional> identity;
        Field<Bundle<HttpResponse>, optional> rejection;
    };
};

} // namespace details

namespace bg::web {

template <typename IDENTITY>
using AuthDecision = Bundle<details::AuthDecisionSchema<IDENTITY>::template schema>;

namespace auth {

using bg::literals::operator""_n;

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> accept(Scalar<IDENTITY> identity) {
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>("identity"_n = identity);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> accept(allocator &memory, Scalar<IDENTITY> identity) {
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>(memory, "identity"_n = identity);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> reject(Scalar<Bundle<HttpResponse>> response) {
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>("rejection"_n = response);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> reject(allocator &memory, Scalar<Bundle<HttpResponse>> response) {
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>(memory, "rejection"_n = response);
}

} // namespace auth

} // namespace bg::web

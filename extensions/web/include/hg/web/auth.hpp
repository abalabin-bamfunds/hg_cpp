#pragma once

#include <hg/scalar/bundle/value.hpp>
#include <hg/optionality.hpp>
#include <hg/util/named_argument.hpp>
#include <hg/web/types.hpp>

namespace hg::web::details {

template <typename IDENTITY> struct AuthDecisionSchema {
    template <template <typename, auto...> typename Field> struct schema {
        Field<IDENTITY, optional> identity;
        Field<Bundle<HttpResponse>, optional> rejection;
    };
};

} // namespace details

namespace hg::web {

template <typename IDENTITY>
using AuthDecision = Bundle<details::AuthDecisionSchema<IDENTITY>::template schema>;

namespace auth {

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> accept(Scalar<IDENTITY> identity) {
    using namespace hg::literals;
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>("identity"_n = identity);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> accept(allocator &memory, Scalar<IDENTITY> identity) {
    using namespace hg::literals;
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>(memory, "identity"_n = identity);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> reject(Scalar<Bundle<HttpResponse>> response) {
    using namespace hg::literals;
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>("rejection"_n = response);
}

template <typename IDENTITY>
[[nodiscard]] ScalarValue<AuthDecision<IDENTITY>> reject(allocator &memory, Scalar<Bundle<HttpResponse>> response) {
    using namespace hg::literals;
    return bundle<details::AuthDecisionSchema<IDENTITY>::template schema>(memory, "rejection"_n = response);
}

} // namespace auth

} // namespace hg::web

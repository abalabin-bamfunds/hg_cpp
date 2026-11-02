#pragma once

#include <hg/export.hpp>
#include <hg/literal/scalar_literal.hpp>
#include <hg/scalar/dispatch.hpp>

namespace hg {

struct ScalarLiteralOperations {
    // The callback constructs an uninitialized destination. It must destroy partial state before throwing. The
    // source literal is borrowed for the call.
    void (*construct)(const literal::ScalarLiteral &, const ScalarRuntimeType &, void *, allocator &);
};

template <> HG_EXPORT ScalarDispatch<ScalarLiteralOperations> &
ScalarDispatch<ScalarLiteralOperations>::instance() noexcept;

} // namespace hg

#pragma once

#include <bg/export.hpp>
#include <bg/literal/scalar_literal.hpp>
#include <bg/scalar/dispatch.hpp>

namespace bg {

struct ScalarLiteralOperations {
    // The callback constructs an uninitialized destination. It must destroy partial state before throwing. The
    // source literal is borrowed for the call.
    void (*construct)(const literal::ScalarLiteral &, const ScalarRuntimeType &, void *, allocator &);
};

template <> BG_EXPORT ScalarDispatch<ScalarLiteralOperations> &
ScalarDispatch<ScalarLiteralOperations>::instance() noexcept;

} // namespace bg

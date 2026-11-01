#pragma once

#include <hg/type/type_resolution_error.hpp>

namespace hg::details {

class ResolverTypeResolutionError final : public TypeResolutionError {
  public:
    // Distinguish a propagated resolver failure from a TypeResolutionError thrown by a requirement.
    explicit ResolverTypeResolutionError(const TypeResolutionError &error) : TypeResolutionError{error} {}
};

} // namespace hg::details

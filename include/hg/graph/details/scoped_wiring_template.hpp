#pragma once

#include <bg/graph/wiring_template.hpp>

#include <functional>
#include <utility>

namespace bg::details {

template <typename ScopeFactory> class ScopedWiringTemplate final : public WiringTemplate {
  public:
    ScopedWiringTemplate(const WiringTemplate &target, ScopeFactory scope_factory)
        : target_{&target}, scope_factory_{std::move(scope_factory)} {}

    const Signature &default_signature() const noexcept final { return target_->signature(); }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        return target_->requirement(arguments);
    }

  private:
    util::symbol label(const ResolvedSignature &signature, std::span<const WiringArgument> arguments,
                       util::symbol default_label) const final {
        return target_->label(signature, arguments, default_label);
    }

    std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                    const WiringArguments &argument_view,
                                    std::span<const WiringArgument> arguments) const final {
        auto scope = std::invoke(scope_factory_, signature);
        return target_->wire(wiring, std::move(signature), argument_view, arguments);
    }

    const WiringTemplate *target_;
    [[no_unique_address]] ScopeFactory scope_factory_;
};

template <typename ScopeFactory>
ScopedWiringTemplate(const WiringTemplate &, ScopeFactory) -> ScopedWiringTemplate<ScopeFactory>;

} // namespace bg::details

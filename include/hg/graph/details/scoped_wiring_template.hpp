#pragma once

#include <hg/graph/wiring_template.hpp>

#include <functional>
#include <utility>

namespace hg::details {

template <typename ScopeFactory> class ScopedWiringTemplate final : public WiringTemplate {
  public:
    ScopedWiringTemplate(const WiringTemplate &target, ScopeFactory scope_factory)
        : target_{&target}, scope_factory_{std::move(scope_factory)} {}

    const Signature &default_signature() const noexcept final { return target_->signature(); }

    RequirementResult requirement(const WiringArguments &arguments) const final {
        return target_->requirement(arguments);
    }

    bool allows_output_inference() const noexcept final { return target_->allows_output_inference(); }

  private:
    std::optional<util::symbol> label(const ResolvedSignature &signature,
                                      std::span<const WiringArgument> arguments) const final {
        return target_->label(signature, arguments);
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

} // namespace hg::details

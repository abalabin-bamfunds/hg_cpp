#pragma once

#include <bg/graph/details/operator_resolution.hpp>
#include <bg/graph/wiring_template.hpp>

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

class Operator : public WiringTemplate {
  public:
    explicit Operator(Signature prototype, std::span<const Variable *const> type_parameters = {});
    ~Operator() override;

    const Signature &default_signature() const final { return prototype_; }

    template <typename Implementation>
        requires std::derived_from<std::remove_cvref_t<Implementation>, WiringTemplate>
    std::remove_cvref_t<Implementation> &overload(Implementation &&implementation) {
        using Stored = std::remove_cvref_t<Implementation>;
        auto stored = std::make_unique<Stored>(std::forward<Implementation>(implementation));
        auto &result = *stored;
        add_overload(std::move(stored));
        return result;
    }

    // Resolve borrowed candidates without caching or wiring. The selected implementation pointer remains borrowed.
    details::OperatorResolutionResult resolve_overloads(
        GraphWiring &wiring, std::span<const WiringArgument> arguments, const TypeMap *seed_type_map,
        std::span<const WiringTemplate *const> candidates) const;

  private:
    void add_overload(std::unique_ptr<WiringTemplate> implementation);

    [[nodiscard]] std::optional<WiringPoint> invoke(GraphWiring &wiring,
                                                    std::span<const WiringArgument> arguments,
                                                    const TypeMap *seed_type_map) const final;
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                                  std::span<const WiringArgument>) const final;

    template <typename CandidateRange>
    auto analyze_overloads(
        GraphWiring &wiring, std::span<const WiringArgument> arguments, const TypeMap *seed_type_map,
        const CandidateRange &candidates, bool record_scalar_reads) const;

    Signature prototype_;
    std::vector<const Variable *> type_parameters_;
    std::vector<std::unique_ptr<WiringTemplate>> overloads_;
    std::shared_ptr<const void> cache_identity_{std::make_shared<const char>()};
};

} // namespace bg

#pragma once

#include <bg/graph/wiring_template.hpp>

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg {

class Operator : public WiringTemplate {
  public:
    explicit Operator(Signature prototype);
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

  private:
    void add_overload(std::unique_ptr<WiringTemplate> implementation);

    struct Cache;

    [[nodiscard]] std::optional<WiringPoint> invoke(GraphWiring &wiring,
                                                    std::span<const WiringArgument> arguments,
                                                    const TypeMap *seed_type_map) const final;
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &, ResolvedSignature, const WiringArguments &,
                                                  std::span<const WiringArgument>) const final;

    Signature prototype_;
    std::vector<std::unique_ptr<WiringTemplate>> overloads_;
    mutable std::unique_ptr<Cache> cache_;
};

} // namespace bg

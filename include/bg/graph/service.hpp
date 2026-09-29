#pragma once

#include <bg/graph/wiring_template.hpp>

#include <cstdint>

namespace bg {

enum class ServiceKind : std::uint8_t { reference, subscription, compute };

class Service : public WiringTemplate {
  public:
    Service(ServiceKind kind, Signature signature);
    ~Service() override;

    ServiceKind kind() const noexcept { return kind_; }
    const Signature &signature() const final { return signature_; }

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  const TypeMap &type_map,
                                                  std::span<const WiringArgument> arguments) const final;

    ServiceKind kind_;
    Signature signature_;
};

} // namespace bg

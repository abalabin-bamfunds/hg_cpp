#pragma once

#include <bg/scalar/value.hpp>
#include <bg/graph/wiring_path.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_map.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace bg {

class ServiceDefinition;
class WiringArgument;
class GraphWiring;
class WiringPoint;

namespace details {

struct ServiceInstance {
    const ServiceDefinition *service;
    ResolvedSignature signature;
    TypeMap type_map;
    std::vector<ScalarValue<>> scalar_arguments;
    bool implementation_wired{};
    bool implementation_requests_wired{};
    bool implementation_replies_wired{};
    bool has_client{};
    std::vector<WiringErrorContext> call_contexts;
};

class ServiceRequirements {
  public:
    ServiceInstance &require(const ServiceDefinition &service, ResolvedSignature signature, const TypeMap &type_map,
                             std::span<const WiringArgument> arguments, const WiringPath *call_path);
    ServiceInstance &require_resolved(const ServiceDefinition &service, ResolvedSignature signature,
                                      const TypeMap &type_map, std::vector<ScalarValue<>> scalar_arguments,
                                      const WiringPath *call_path);

    std::span<std::unique_ptr<ServiceInstance>> instances() noexcept { return instances_; }
    std::span<const std::unique_ptr<ServiceInstance>> instances() const noexcept { return instances_; }

  private:
    std::vector<std::unique_ptr<ServiceInstance>> instances_;
};

} // namespace details
} // namespace bg

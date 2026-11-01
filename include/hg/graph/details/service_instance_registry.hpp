#pragma once

#include <hg/scalar/value.hpp>
#include <hg/graph/wiring_path.hpp>
#include <hg/graph/wiring_error.hpp>
#include <hg/type/signature.hpp>
#include <hg/type/type_map.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace hg {

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

class ServiceInstanceRegistry {
  public:
    ServiceInstance &find_or_create(const ServiceDefinition &service, ResolvedSignature signature,
                                    const TypeMap &type_map, std::span<const WiringArgument> arguments,
                                    const WiringPath *call_path);
    ServiceInstance &find_or_create(const ServiceDefinition &service, ResolvedSignature signature,
                                    const TypeMap &type_map, std::vector<ScalarValue<>> scalar_arguments,
                                    const WiringPath *call_path);

    std::span<std::unique_ptr<ServiceInstance>> instances() noexcept { return instances_; }
    std::span<const std::unique_ptr<ServiceInstance>> instances() const noexcept { return instances_; }

  private:
    std::vector<std::unique_ptr<ServiceInstance>> instances_;
};

} // namespace details
} // namespace hg

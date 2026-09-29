#pragma once

#include <bg/scalar/value.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/type_map.hpp>

#include <memory>
#include <span>
#include <vector>

namespace bg {

class Service;
class WiringArgument;

namespace details {

struct ServiceInstance {
    const Service *service;
    ResolvedSignature signature;
    TypeMap type_map;
    std::vector<ScalarValue<>> scalar_arguments;
    bool built{};
};

class ServicePlan {
  public:
    ServiceInstance &require(const Service &service, ResolvedSignature signature, const TypeMap &type_map,
                             std::span<const WiringArgument> arguments);

    std::span<std::unique_ptr<ServiceInstance>> instances() noexcept { return instances_; }
    std::span<const std::unique_ptr<ServiceInstance>> instances() const noexcept { return instances_; }

  private:
    std::vector<std::unique_ptr<ServiceInstance>> instances_;
};

} // namespace details
} // namespace bg

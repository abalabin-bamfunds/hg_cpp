#pragma once

#include <bg/graph/service.hpp>

#include <span>

namespace bg {
namespace details {

class ServiceImplementationAccess {
  public:
    virtual ~ServiceImplementationAccess() = default;

    virtual WiringPoint requests(const ServiceDefinition &service, const TypeMap &type_map,
                                 std::span<const WiringArgument> scalar_arguments) = 0;
    virtual void replies(const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
                         std::span<const WiringArgument> scalar_arguments) = 0;
};

} // namespace details
} // namespace bg

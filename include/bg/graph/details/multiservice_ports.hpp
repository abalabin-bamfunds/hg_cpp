#pragma once

#include <bg/graph/service.hpp>

#include <optional>
#include <span>

namespace bg::details {

class MultiservicePorts {
  public:
    virtual ~MultiservicePorts() = default;

    virtual WiringPoint requests(const ServiceDefinition &service, const TypeMap &type_map,
                                 std::span<const WiringArgument> scalar_arguments,
                                 std::optional<util::symbol> lifted_scalar) = 0;
    virtual void replies(const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
                         std::span<const WiringArgument> scalar_arguments,
                         std::optional<util::symbol> lifted_scalar) = 0;
};

} // namespace bg::details

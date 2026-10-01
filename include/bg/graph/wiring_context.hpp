#pragma once

#include <bg/graph/wiring_path.hpp>

namespace bg {

class GraphWiring;
class ServiceDefinition;
class ServiceCatalogue;
namespace details {
class ServiceImplementationAccess;
} // namespace details

class WiringContext {
  public:
    explicit WiringContext(GraphWiring &wiring);
    ~WiringContext();

    WiringContext(const WiringContext &) = delete;
    WiringContext &operator=(const WiringContext &) = delete;
    WiringContext(WiringContext &&) = delete;
    WiringContext &operator=(WiringContext &&) = delete;

    static WiringContext &current();

    GraphWiring &wiring() const noexcept { return *wiring_; }

    const WiringPath *path() const noexcept { return path_; }

  private:
    friend class GraphWiring;
    friend class ServiceDefinition;
    friend class ServiceCatalogue;

    WiringContext(GraphWiring &wiring, const WiringPath &path) noexcept;
    WiringContext(GraphWiring &wiring, details::ServiceImplementationAccess &service_implementation) noexcept;
    details::ServiceImplementationAccess &service_implementation() const;

    GraphWiring *wiring_;
    const WiringPath *path_{};
    WiringContext *previous_;
    details::ServiceImplementationAccess *service_implementation_{};
};

} // namespace bg

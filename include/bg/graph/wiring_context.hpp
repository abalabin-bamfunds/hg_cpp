#pragma once

#include <bg/graph/wiring_path.hpp>

namespace bg {

class GraphWiring;
namespace details {
class MultiservicePorts;
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

    WiringContext(GraphWiring &wiring, const WiringPath &path) noexcept;
    WiringContext(GraphWiring &wiring, details::MultiservicePorts &multiservice_ports) noexcept;

    GraphWiring &wiring() const noexcept { return *wiring_; }

    const WiringPath *path() const noexcept { return path_; }

    details::MultiservicePorts &multiservice_ports() const;

  private:
    GraphWiring *wiring_;
    const WiringPath *path_{};
    WiringContext *previous_;
    details::MultiservicePorts *multiservice_ports_{};
};

} // namespace bg

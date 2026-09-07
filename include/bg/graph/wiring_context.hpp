#pragma once

#include <bg/graph/wiring_path.hpp>

namespace bg {

class GraphWiring;

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

    WiringContext(GraphWiring &wiring, const WiringPath &path) noexcept;

    GraphWiring *wiring_;
    const WiringPath *path_{};
    WiringContext *previous_;
};

} // namespace bg

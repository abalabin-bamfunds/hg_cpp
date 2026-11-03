#pragma once

#include <hg/graph/wiring_error.hpp>
#include <hg/graph/wiring_path.hpp>

#include <span>
#include <vector>

namespace hg {

class GraphWiring;
namespace details {
class MultiservicePorts;

class WiringDiagnosticContext {
  public:
    WiringDiagnosticContext(GraphWiring &wiring, std::span<const WiringErrorContext> contexts);
    ~WiringDiagnosticContext();

    WiringDiagnosticContext(const WiringDiagnosticContext &) = delete;
    WiringDiagnosticContext &operator=(const WiringDiagnosticContext &) = delete;
    WiringDiagnosticContext(WiringDiagnosticContext &&) = delete;
    WiringDiagnosticContext &operator=(WiringDiagnosticContext &&) = delete;

    static std::span<const WiringErrorContext> current(const GraphWiring &wiring) noexcept;

  private:
    GraphWiring *wiring_;
    std::vector<WiringErrorContext> contexts_;
    WiringDiagnosticContext *previous_;
};
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

    WiringContext(GraphWiring &wiring, const WiringPath &path, bool generated_label = false) noexcept;
    WiringContext(GraphWiring &wiring, details::MultiservicePorts &multiservice_ports) noexcept;
    WiringContext(GraphWiring &wiring, const WiringPath &path, details::MultiservicePorts &multiservice_ports,
                  bool generated_label = false) noexcept;

    GraphWiring &wiring() const noexcept { return *wiring_; }

    const WiringPath *path() const noexcept { return path_; }
    bool generated_label() const noexcept { return generated_label_; }

    details::MultiservicePorts &multiservice_ports() const;
    details::MultiservicePorts *multiservice_ports_if_available() const noexcept { return multiservice_ports_; }

  private:
    GraphWiring *wiring_;
    const WiringPath *path_{};
    bool generated_label_{};
    WiringContext *previous_;
    details::MultiservicePorts *multiservice_ports_{};
};

} // namespace hg

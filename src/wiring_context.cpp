#include <bg/graph/wiring_context.hpp>

#include <bg/graph/wiring_error.hpp>

#include "thread_local_pointer.hpp"

#include <algorithm>

namespace bg {
namespace {

util::thread_local_pointer &active_context() {
    static util::thread_local_pointer value;

    return value;
}

util::thread_local_pointer &active_diagnostic_context() {
    static util::thread_local_pointer value;

    return value;
}

} // namespace

details::WiringDiagnosticContext::WiringDiagnosticContext(GraphWiring &wiring,
                                                         std::span<const WiringErrorContext> contexts)
    : wiring_{&wiring}, previous_{static_cast<details::WiringDiagnosticContext *>(
                                    active_diagnostic_context().get())} {
    if (previous_ != nullptr && previous_->wiring_ == &wiring) {
        contexts_ = previous_->contexts_;
    }
    for (const auto &context : contexts) {
        if (std::ranges::find(contexts_, context) == contexts_.end()) {
            contexts_.push_back(context);
        }
    }
    active_diagnostic_context().set(this);
}

details::WiringDiagnosticContext::~WiringDiagnosticContext() {
    active_diagnostic_context().set(previous_);
}

std::span<const WiringErrorContext>
details::WiringDiagnosticContext::current(const GraphWiring &wiring) noexcept {
    const auto *context = static_cast<const details::WiringDiagnosticContext *>(active_diagnostic_context().get());
    return context == nullptr || context->wiring_ != &wiring ? std::span<const WiringErrorContext>{}
                                                              : std::span{context->contexts_};
}

WiringContext::WiringContext(GraphWiring &wiring)
    : wiring_(&wiring), path_(nullptr), previous_(static_cast<WiringContext *>(active_context().get())),
      multiservice_ports_(previous_ == nullptr || previous_->wiring_ != &wiring ? nullptr
                                                                               : previous_->multiservice_ports_) {
    active_context().set(this);
}

WiringContext::WiringContext(GraphWiring &wiring, const WiringPath &path) noexcept
    : wiring_(&wiring), path_(&path), previous_(static_cast<WiringContext *>(active_context().get())),
      multiservice_ports_(previous_ == nullptr || previous_->wiring_ != &wiring ? nullptr
                                                                               : previous_->multiservice_ports_) {
    active_context().set(this);
}

WiringContext::WiringContext(GraphWiring &wiring, details::MultiservicePorts &multiservice_ports) noexcept
    : wiring_(&wiring), path_(nullptr), previous_(static_cast<WiringContext *>(active_context().get())),
      multiservice_ports_(&multiservice_ports) {
    active_context().set(this);
}

WiringContext::WiringContext(GraphWiring &wiring, const WiringPath &path,
                             details::MultiservicePorts &multiservice_ports) noexcept
    : wiring_(&wiring), path_(&path), previous_(static_cast<WiringContext *>(active_context().get())),
      multiservice_ports_(&multiservice_ports) {
    active_context().set(this);
}

WiringContext::~WiringContext() {
    active_context().set(previous_);
}

WiringContext &WiringContext::current() {
    auto *context = static_cast<WiringContext *>(active_context().get());
    if (context == nullptr) {
        throw WiringError{"a wiring-template call requires an active WiringContext"};
    }

    return *context;
}

details::MultiservicePorts &WiringContext::multiservice_ports() const {
    if (multiservice_ports_ == nullptr) {
        throw WiringError{"multiservice ports are available only while wiring a multiservice graph"};
    }
    return *multiservice_ports_;
}

} // namespace bg

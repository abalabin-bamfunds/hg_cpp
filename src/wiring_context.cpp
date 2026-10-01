#include <bg/graph/wiring_context.hpp>

#include <bg/graph/wiring_error.hpp>

#include "thread_local_pointer.hpp"

namespace bg {
namespace {

util::thread_local_pointer &active_context() {
    static util::thread_local_pointer value;

    return value;
}

} // namespace

WiringContext::WiringContext(GraphWiring &wiring)
    : wiring_(&wiring), path_(nullptr), previous_(static_cast<WiringContext *>(active_context().get())),
      service_implementation_(
          previous_ == nullptr || previous_->wiring_ != &wiring ? nullptr : previous_->service_implementation_) {
    active_context().set(this);
}

WiringContext::WiringContext(GraphWiring &wiring, const WiringPath &path) noexcept
    : wiring_(&wiring), path_(&path), previous_(static_cast<WiringContext *>(active_context().get())),
      service_implementation_(
          previous_ == nullptr || previous_->wiring_ != &wiring ? nullptr : previous_->service_implementation_) {
    active_context().set(this);
}

WiringContext::WiringContext(GraphWiring &wiring, details::ServiceImplementationAccess &service_implementation) noexcept
    : wiring_(&wiring), path_(nullptr), previous_(static_cast<WiringContext *>(active_context().get())),
      service_implementation_(&service_implementation) {
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

details::ServiceImplementationAccess &WiringContext::service_implementation() const {
    if (service_implementation_ == nullptr) {
        throw WiringError{"service implementation ports are available only while wiring a multiservice graph"};
    }
    return *service_implementation_;
}

} // namespace bg

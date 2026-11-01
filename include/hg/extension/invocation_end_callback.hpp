#pragma once

#include <hg/export.hpp>

namespace hg {

class EvaluationContext;

namespace details {
struct InvocationEndCallback;
class InvocationEndCallbackScope;
} // namespace details

namespace extension {

// Deduplicates an invocation-end callback while it is registered. Destroying the owner cancels the callback.
class HG_EXPORT InvocationEndRegistration {
  public:
    InvocationEndRegistration() noexcept = default;
    ~InvocationEndRegistration();

    InvocationEndRegistration(const InvocationEndRegistration &) = delete;
    InvocationEndRegistration &operator=(const InvocationEndRegistration &) = delete;
    InvocationEndRegistration(InvocationEndRegistration &&) = delete;
    InvocationEndRegistration &operator=(InvocationEndRegistration &&) = delete;

    void cancel() noexcept;

  private:
    friend class details::InvocationEndCallbackScope;

    void attach(details::InvocationEndCallback *callback) noexcept;

    details::InvocationEndCallback *callback_{};
};

} // namespace extension
} // namespace hg

#pragma once

#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/util/symbol.hpp>

#include <cstdint>
#include <vector>

namespace bg {

using service_client_id_t = std::int64_t;

class ServiceDefinition : public WiringTemplate {
  public:
    explicit ServiceDefinition(Signature signature);
    ~ServiceDefinition() override;

    const Signature &default_signature() const final { return signature_; }

    WiringPoint implementation_requests(const TypeMap &type_map,
                                        std::span<const WiringArgument> scalar_arguments) const;
    void implementation_replies(const TypeMap &type_map, WiringPoint replies,
                                std::span<const WiringArgument> scalar_arguments) const;

    virtual bool implementation_requests_required() const noexcept = 0;
    virtual const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const = 0;
    virtual WiringPoint wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                                     details::NestedBindingIdentity identity,
                                                     bool bind_clients) const = 0;
    virtual void append_implementation_request_arguments(WiringPoint requests,
                                                         std::vector<WiringArgument> &arguments) const = 0;

  private:
    [[nodiscard]] std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature,
                                                  const WiringArguments &argument_view,
                                                  std::span<const WiringArgument> arguments) const final;
    virtual std::optional<WiringPoint> wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                                   const TypeMap &type_map,
                                                   std::span<const WiringArgument> arguments) const = 0;

    Signature signature_;
};

class Service : public ServiceDefinition {
  public:
    static constexpr bool supports_implementation_requests = true;

    explicit Service(Signature signature);

    bool implementation_requests_required() const noexcept final { return true; }
    const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const final;
    WiringPoint wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                             details::NestedBindingIdentity identity,
                                             bool bind_clients) const final;
    void append_implementation_request_arguments(WiringPoint requests,
                                                 std::vector<WiringArgument> &arguments) const final;

  private:
    std::optional<WiringPoint> wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                           const TypeMap &type_map,
                                           std::span<const WiringArgument> arguments) const final;
};

class ReferenceService : public ServiceDefinition {
  public:
    static constexpr bool supports_implementation_requests = false;

    explicit ReferenceService(Signature signature);

    bool implementation_requests_required() const noexcept final { return false; }
    const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const final;
    WiringPoint wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                             details::NestedBindingIdentity identity,
                                             bool bind_clients) const final;
    void append_implementation_request_arguments(WiringPoint requests,
                                                 std::vector<WiringArgument> &arguments) const final;

  private:
    std::optional<WiringPoint> wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                           const TypeMap &type_map,
                                           std::span<const WiringArgument> arguments) const final;
};

class SubscriptionService : public ServiceDefinition {
  public:
    static constexpr bool supports_implementation_requests = true;

    explicit SubscriptionService(Signature signature);

    bool implementation_requests_required() const noexcept final { return true; }
    const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const final;
    WiringPoint wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                             details::NestedBindingIdentity identity,
                                             bool bind_clients) const final;
    void append_implementation_request_arguments(WiringPoint requests,
                                                 std::vector<WiringArgument> &arguments) const final;

  private:
    std::optional<WiringPoint> wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                           const TypeMap &type_map,
                                           std::span<const WiringArgument> arguments) const final;
};

} // namespace bg

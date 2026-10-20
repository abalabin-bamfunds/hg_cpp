#pragma once

#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/type_mapping.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/util/symbol.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace bg {

using service_client_id_t = std::int64_t;

struct ServiceImplementationRequestInput {
    std::optional<util::symbol> name;
    const TimeSeriesType *type;
};

class ServiceDefinition : public WiringTemplate {
  public:
    explicit ServiceDefinition(Signature signature);
    ~ServiceDefinition() override;

    const Signature &default_signature() const final { return signature_; }

    WiringPoint implementation_requests(const TypeMap &type_map,
                                        std::span<const WiringArgument> scalar_arguments) const;
    WiringPoint implementation_requests_for_catch_all(const TypeMap &type_map, util::symbol lifted_scalar,
                                                      std::span<const WiringArgument> scalar_arguments) const;
    void implementation_replies(const TypeMap &type_map, WiringPoint replies,
                                std::span<const WiringArgument> scalar_arguments) const;
    void implementation_replies_for_catch_all(const TypeMap &type_map, WiringPoint replies,
                                               util::symbol lifted_scalar,
                                               std::span<const WiringArgument> scalar_arguments) const;

    virtual bool implementation_requests_required() const noexcept = 0;
    virtual std::vector<ServiceImplementationRequestInput>
    implementation_request_inputs(const ResolvedSignature &signature) const = 0;
    virtual const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const = 0;
    virtual std::vector<std::size_t> lifted_scalar_parameters(const Signature &) const { return {}; }
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
    std::vector<ServiceImplementationRequestInput>
    implementation_request_inputs(const ResolvedSignature &signature) const final;
    const TimeSeriesType *implementation_output_type(const ResolvedSignature &signature) const final;
    std::vector<std::size_t> lifted_scalar_parameters(const Signature &implementation) const final;
    WiringPoint wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                             details::NestedBindingIdentity identity,
                                             bool bind_clients) const final;
    void append_implementation_request_arguments(WiringPoint requests,
                                                 std::vector<WiringArgument> &arguments) const final;
    WiringPoint wire_catch_all_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                        details::NestedBindingIdentity identity,
                                        std::span<const std::size_t> lifted_parameters,
                                        bool bind_clients = true) const;
    void wire_forwarding_requests(GraphWiring &wiring, details::NestedBindingIdentity original_identity,
                                  details::NestedBindingIdentity catch_all_identity,
                                  std::span<const WiringArgument> lifted_scalars) const;

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
    std::vector<ServiceImplementationRequestInput>
    implementation_request_inputs(const ResolvedSignature &signature) const final;
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
    std::vector<ServiceImplementationRequestInput>
    implementation_request_inputs(const ResolvedSignature &signature) const final;
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

#include <bg/graph/service.hpp>

#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/details/node_scheduler.hpp>
#include <bg/graph/details/service_binding_section.hpp>
#include <bg/graph/details/multiservice_ports.hpp>
#include <bg/graph/details/service_requirements.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/operator.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node.hpp>
#include <bg/node/details/output_factory.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_variable.hpp>
#include <bg/time_series/tsb/tsb_output.hpp>
#include <bg/time_series/tsd/tsd_output.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/util/occupancy_index.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg {
namespace details {

class ServiceRequestBinding {
  public:
    static ServiceRequestBinding &at(EvaluationContext &context, std::size_t binding_index) {
        const auto *section =
            static_cast<const ServiceBindingSection *>(context.find_section(&service_binding_section_type));
        assert(section != nullptr && binding_index < section->bindings.size());
        return *section->bindings[binding_index];
    }

    void initialize(const EvaluationContext &context, const TimeSeriesRuntimeType &runtime_type) noexcept {
        provider_ = NodeScheduler{context};
        runtime_type_ = &runtime_type;
    }

    template <typename TimeSeries, typename Function>
    void modify(EvaluationContext &caller_context, Function &&function) {
        assert(provider_ && runtime_type_ != nullptr);

        auto &graph = provider_.runtime_graph();
        const auto node_index = provider_.node_index();
        auto runtime_node = graph.node(node_index);
        const auto *inputs = &caller_context.runtime_graph() == &graph ? caller_context.runtime_graph_inputs()
                                                                       : nullptr;
        auto provider_context = provider_.node_context(caller_context, inputs);
        auto *bundle_owner = current_bundle_allocation_owner();
        assert(bundle_owner != nullptr);
        const auto allocation_context = ScalarAllocationContext{graph.allocation(), *bundle_owner, &graph, node_index};
        auto output = output_factory::make<Out<TimeSeries>>(runtime_node.output_last_updated(),
                                                            runtime_node.output_value(), *runtime_type_,
                                                            provider_context);

        if (std::invoke(std::forward<Function>(function), output)) {
            provider_.schedule();
        }
    }

  private:
    NodeScheduler provider_;
    const TimeSeriesRuntimeType *runtime_type_{};
};

} // namespace details

namespace {

using SERVICE_KEY = ScalarVariable<"SERVICE_KEY", EqualityComparable, Hashable>;
using SERVICE_REQUESTS = TimeSeriesVariable<"SERVICE_REQUESTS">;

struct SubscriptionRequestClientState {
    details::ServiceRequestBinding *requests{};
    item_id_t key_id{key_set_item_id};
};

struct SubscriptionRequestClient {
    In<TS<SERVICE_KEY, TimeSeriesGuarantee::final>> key;
    Scalar<Opaque<std::size_t>> binding_index;
    State<SubscriptionRequestClientState> state;

    static constexpr bool deduplicate_wiring = false;

    void start(EvaluationContext *context) const {
        state->requests = &details::ServiceRequestBinding::at(*context, binding_index.value());
    }

    void evaluate(EvaluationContext *context) const {
        assert(key.valid());
        assert(state->key_id == key_set_item_id);

        const auto value = key.value_view();
        state->requests->modify<TSD<SERVICE_KEY, TS<std::int64_t>>>(*context, [&](auto &output) {
            auto count = output[value];
            count.set(count.valid() ? count.value() + 1 : 1);
            state->key_id = output.item_id(value);
            return true;
        });
    }

    void on_invalidation(EvaluationContext *context) const { withdraw(*context); }

    void stop(EvaluationContext *context) const noexcept { withdraw(*context); }

  private:
    void withdraw(EvaluationContext &context) const {
        if (state->key_id == key_set_item_id) {
            return;
        }

        state->requests->modify<TSD<SERVICE_KEY, TS<std::int64_t>>>(context, [&](auto &output) {
            const auto old_key = output.key(state->key_id);
            auto count = output[old_key];
            assert(count.valid());
            const auto count_value = count.value();
            assert(count_value > 0);
            if (count_value == 1) {
                output.erase(old_key);
            } else {
                count.set(count_value - 1);
            }

            state->key_id = key_set_item_id;
            return true;
        });
    }

};

struct ServiceRequestProvider {
    State<details::ServiceRequestBinding> binding;
    Out<SERVICE_REQUESTS> output;

    void start(EvaluationContext *context) const {
        binding->initialize(*context, output.runtime_type());
        output.visit(
            [](Out<TSD<SCALAR, TS_T>> *requests) { requests->clear(); },
            [](Out<TSB<SCHEMA>> *requests) {
                for (auto index = std::size_t{}; index < requests->size(); ++index) {
                    (*requests)[index].visit([](Out<TSD<SCALAR, TS_T>> *field) { field->clear(); }, ddv::noop);
                }
            },
            ddv::noop);
    }

    void evaluate() const {}
};

const auto subscription_request_client = node<SubscriptionRequestClient>();
const auto service_request_provider = node<ServiceRequestProvider>();

using SERVICE_ARGUMENT = TimeSeriesVariable<"SERVICE_ARGUMENT">;

struct ServiceRequestClientState {
    details::ServiceRequestBinding *requests{};
    service_client_id_t id{};
    bool active{};
};

class ServiceRequestClient {
  public:
    using compiled_data_type = std::size_t;
    using state_type = ServiceRequestClientState;
    static constexpr bool deduplicate_wiring = false;

    ServiceRequestClient(const ResolvedSignature &service_signature, std::size_t binding_index)
        : signature_{make_signature(service_signature)}, binding_index_{binding_index} {}

    const Signature &signature() const noexcept { return signature_; }
    NodeTemplateIdentity definition_identity() const {
        static constexpr unsigned char identity{};
        return NodeTemplateIdentity::persistent(&identity);
    }

    compiled_data_type compile(const ResolvedSignature &, allocator &) const noexcept { return binding_index_; }

    static void start(NodeInvocation<compiled_data_type, state_type> invocation) {
        invocation.state().requests =
            &details::ServiceRequestBinding::at(invocation.context(), invocation.compiled_data());
    }

    static void evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        const auto parameter_count = invocation.parameter_count() / 2;
        auto any_valid = false;
        for (auto input_index = std::size_t{}; input_index < parameter_count; ++input_index) {
            any_valid |= invocation.input<In<SERVICE_ARGUMENT>>(input_index * 2).valid();
        }

        auto output = invocation.output<TS<service_client_id_t, TimeSeriesGuarantee::deduplicated>>();
        if (!any_valid && !state.active) {
            if (state.id != 0) {
                output.invalidate();
            }
            return;
        }

        if (state.id == 0) {
            state.id = next_client_id();
        }
        update_requests(invocation, any_valid);

        if (any_valid) {
            if (!state.active) {
                output.set(state.id);
            }
        } else {
            output.invalidate();
        }
        state.active = any_valid;
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        auto &state = invocation.state();
        if (state.requests != nullptr && state.active) {
            update_requests(invocation, false);
            state.active = false;
        }
    }

  private:
    static service_client_id_t next_client_id() noexcept {
        static auto next = std::atomic<service_client_id_t>{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    static void update_requests(NodeInvocation<compiled_data_type, state_type> invocation, bool active) {
        auto &state = invocation.state();
        const auto client_id = ScalarValue<service_client_id_t>{state.id};
        state.requests->modify<TSB<SCHEMA>>(invocation.context(), [&](auto &requests) {
            const auto parameter_count = invocation.parameter_count() / 2;
            auto changed = false;
            for (auto parameter = std::size_t{}; parameter < parameter_count; ++parameter) {
                requests[parameter].visit(
                    [&](Out<TSD<SCALAR, TS_T>> *dictionary) {
                        const auto key = Scalar<SCALAR>::from(client_id);
                        if (!active) {
                            changed |= dictionary->erase(key);
                            return;
                        }

                        const auto reference =
                            invocation.input<In<REF<SERVICE_ARGUMENT>>>(parameter * 2 + 1).value();
                        if (!reference && dictionary->item_id(key) == util::occupancy_index::npos) {
                            changed = true;
                        }
                        auto item = (*dictionary)[key];
                        item.visit(
                            [&](Out<REF<TS_T>> *output) {
                                if (!reference) {
                                    if (output->valid()) {
                                        output->invalidate();
                                        changed = true;
                                    }
                                    return;
                                }

                                if (!output->valid() || output->value() != reference) {
                                    output->set_value(reference);
                                    changed = true;
                                }
                            },
                            ddv::noop);
                    },
                    ddv::noop);
            }
            return changed;
        });
    }

    static Signature make_signature(const ResolvedSignature &service_signature) {
        auto parameters = std::vector<Parameter>{};
        auto time_series_index = std::size_t{};
        for (const auto &parameter : service_signature.parameters) {
            if (!parameter.type->is_time_series()) {
                continue;
            }

            const auto &time_series_type = static_cast<const TimeSeriesType &>(*parameter.type);
            parameters.push_back({parameter.name, &time_series_type, active, optional});
            const auto &reference_type = time_series_reference_type(time_series_type, TimeSeriesGuarantee::ordinary);
            const auto name = util::symbol{"reference_" + std::to_string(time_series_index++)};
            parameters.push_back({name, &reference_type, active, optional});
        }

        return Signature{util::symbol{"service_client"}, std::move(parameters),
                         TS<service_client_id_t, TimeSeriesGuarantee::deduplicated>::type()};
    }

    Signature signature_;
    std::size_t binding_index_;
};

} // namespace

ServiceDefinition::ServiceDefinition(Signature signature) : signature_{std::move(signature)} {
    auto scalar_parameters_started = false;
    for (auto index = std::size_t{}; index < signature_.parameter_count(); ++index) {
        const auto &type = *signature_.parameter(index).type;
        if (type.is_scalar()) {
            scalar_parameters_started = true;
            continue;
        }
        if (!type.is_time_series()) {
            throw std::invalid_argument{"a service parameter must be a time series or scalar"};
        }
        if (scalar_parameters_started) {
            throw std::invalid_argument{"a service time series parameter cannot follow a scalar parameter"};
        }
        if (static_cast<const TimeSeriesType &>(type).contains_reference()) {
            throw std::invalid_argument{"service signatures cannot contain reference types"};
        }
    }
    if (signature_.output() != nullptr && signature_.output()->contains_reference()) {
        throw std::invalid_argument{"service signatures cannot contain reference types"};
    }
}

ServiceDefinition::~ServiceDefinition() = default;

WiringPoint ServiceDefinition::implementation_requests(const TypeMap &type_map,
                                                       std::span<const WiringArgument> scalar_arguments) const {
    if (!implementation_requests_required()) {
        throw WiringError{"reference services do not have request time series"};
    }
    return WiringContext::current().multiservice_ports().requests(*this, type_map, scalar_arguments);
}

void ServiceDefinition::implementation_replies(const TypeMap &type_map, WiringPoint replies,
                                               std::span<const WiringArgument> scalar_arguments) const {
    if (!signature_.has_output()) {
        throw WiringError{"an outputless service cannot be assigned replies"};
    }
    WiringContext::current().multiservice_ports().replies(*this, type_map, replies, scalar_arguments);
}

std::optional<WiringPoint> ServiceDefinition::wire(GraphWiring &wiring, ResolvedSignature signature,
                                                   const WiringArguments &argument_view,
                                                   std::span<const WiringArgument> arguments) const {
    for (const auto &parameter : signature.parameters) {
        if (const auto *time_series = type_cast<TimeSeriesType>(parameter.type);
            time_series != nullptr && time_series->contains_reference()) {
            throw WiringError{"service signatures cannot contain reference types"};
        }
    }
    if (signature.output != nullptr && signature.output->contains_reference()) {
        throw WiringError{"service signatures cannot contain reference types"};
    }

    return wire_client(wiring, std::move(signature), argument_view.type_map(), arguments);
}

Service::Service(Signature signature) : ServiceDefinition{std::move(signature)} {
    auto has_time_series_parameter = false;
    for (auto index = std::size_t{}; index < this->signature().parameter_count(); ++index) {
        has_time_series_parameter |= this->signature().parameter(index).type->is_time_series();
    }
    if (!has_time_series_parameter) {
        throw std::invalid_argument{"a service requires at least one time series parameter"};
    }
}

std::vector<ServiceImplementationRequestInput>
Service::implementation_request_inputs(const ResolvedSignature &signature) const {
    auto inputs = std::vector<ServiceImplementationRequestInput>{};
    const auto &client_id_type = cpp_scalar_type<service_client_id_t>();
    for (const auto &parameter : signature.parameters) {
        if (const auto *type = type_cast<TimeSeriesType>(parameter.type)) {
            inputs.push_back({parameter.name,
                              &time_series_dictionary_type(
                                  client_id_type, type->with_guarantee_limit(TimeSeriesGuarantee::ordinary),
                                  TimeSeriesGuarantee::ordinary)});
        }
    }
    return inputs;
}

const TimeSeriesType *Service::implementation_output_type(const ResolvedSignature &signature) const {
    return signature.output == nullptr
               ? nullptr
               : &time_series_dictionary_type(cpp_scalar_type<service_client_id_t>(), *signature.output,
                                              TimeSeriesGuarantee::ordinary);
}

WiringPoint Service::wire_implementation_requests(GraphWiring &wiring, const ResolvedSignature &signature,
                                                  details::NestedBindingIdentity identity, bool bind_clients) const {
    const auto &client_id_type = cpp_scalar_type<service_client_id_t>();
    auto fields = std::vector<SchemaField>{};
    for (const auto &parameter : signature.parameters) {
        if (!parameter.type->is_time_series()) {
            continue;
        }

        const auto &value_type = static_cast<const TimeSeriesType &>(*parameter.type);
        const auto &reference_type = time_series_reference_type(value_type, TimeSeriesGuarantee::ordinary);
        const auto &request_type =
            time_series_dictionary_type(client_id_type, reference_type, TimeSeriesGuarantee::ordinary);
        fields.push_back({parameter.name, &request_type});
    }

    const auto &request_type = TimeSeriesBundleType::from_fields(fields);
    auto type_map = TypeMap{};
    type_map.set(details::TimeSeriesVariableInstance<SERVICE_REQUESTS>::get(), &request_type);
    const auto output = static_cast<WiringPoint>(service_request_provider(type_map));
    if (bind_clients) {
        wiring.provide_service_binding(identity, output);
    }
    return output;
}

void Service::append_implementation_request_arguments(WiringPoint requests,
                                                      std::vector<WiringArgument> &arguments) const {
    details::append_wiring_arguments(arguments, kwargs(requests));
}

std::optional<WiringPoint> Service::wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                                const TypeMap &type_map,
                                                std::span<const WiringArgument> arguments) const {
    auto &instance = wiring.root_service_requirements().require(*this, signature, type_map, arguments,
                                                                WiringContext::current().path());
    const auto identity = details::NestedBindingIdentity::object(&instance);
    const auto binding_index =
        wiring.require_service_binding(identity, GraphWiring::ServiceBindingOrdering::provider_after_consumers);
    auto client_arguments = std::vector<WiringArgument>{};
    for (auto index = std::size_t{}; index < signature.parameters.size(); ++index) {
        if (!signature.parameters[index].type->is_time_series()) {
            continue;
        }

        const auto &input = arguments[index].time_series();
        client_arguments.emplace_back(input);
        client_arguments.emplace_back(input);
    }

    const auto &client_template =
        wiring.own_node_template(std::make_unique<RuntimeSignatureNodeTemplate<ServiceRequestClient>>(
            ServiceRequestClient{signature, binding_index}));
    auto client_call = wiring.invoke_node(client_template, client_arguments);
    if (!client_call.output) {
        throw WiringError{"a service client did not produce its client id"};
    }
    const auto client = client_call.output;
    wiring.register_service_binding_consumer(identity, client_call.identity);
    if (signature.output == nullptr) {
        return std::nullopt;
    }

    const auto &response_type = *implementation_output_type(signature);
    const auto response = wiring.require_inherited_time_series(identity, signature.name, response_type);
    return operators::getitem(response, *client);
}

ReferenceService::ReferenceService(Signature signature) : ServiceDefinition{std::move(signature)} {
    auto has_time_series_parameter = false;
    for (auto index = std::size_t{}; index < this->signature().parameter_count(); ++index) {
        has_time_series_parameter |= this->signature().parameter(index).type->is_time_series();
    }
    if (has_time_series_parameter || !this->signature().has_output()) {
        throw std::invalid_argument{"a reference service requires a source-like signature"};
    }
}

std::vector<ServiceImplementationRequestInput>
ReferenceService::implementation_request_inputs(const ResolvedSignature &) const {
    return {};
}

const TimeSeriesType *ReferenceService::implementation_output_type(const ResolvedSignature &signature) const {
    return signature.output;
}

WiringPoint ReferenceService::wire_implementation_requests(GraphWiring &, const ResolvedSignature &,
                                                           details::NestedBindingIdentity, bool) const {
    throw WiringError{"reference services do not have request time series"};
}

void ReferenceService::append_implementation_request_arguments(WiringPoint,
                                                               std::vector<WiringArgument> &) const {}

std::optional<WiringPoint> ReferenceService::wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                                         const TypeMap &type_map,
                                                         std::span<const WiringArgument> arguments) const {
    return wiring.require_service(*this, std::move(signature), type_map, arguments);
}

SubscriptionService::SubscriptionService(Signature signature) : ServiceDefinition{std::move(signature)} {
    const auto *key_type = this->signature().parameter_count() != 0
                               ? type_cast<TimeSeriesValueType>(this->signature().parameter(0).type)
                               : nullptr;
    auto time_series_parameters = std::size_t{};
    for (auto index = std::size_t{}; index < this->signature().parameter_count(); ++index) {
        time_series_parameters += this->signature().parameter(index).type->is_time_series();
    }
    if (time_series_parameters != 1 || key_type == nullptr || !this->signature().has_output()) {
        throw std::invalid_argument{"a subscription service requires one time series value parameter and an output"};
    }

    auto declared_guarantee = key_type->guarantee().value();
    if (!declared_guarantee) {
        const auto *variable = key_type->guarantee().variable();
        if (variable != nullptr) {
            declared_guarantee = variable->minimum();
        }
    }
    if (!declared_guarantee || !satisfies(*declared_guarantee, TimeSeriesGuarantee::final)) {
        throw std::invalid_argument{"a subscription service key must have a final guarantee"};
    }
}

std::vector<ServiceImplementationRequestInput>
SubscriptionService::implementation_request_inputs(const ResolvedSignature &signature) const {
    const auto &key_type = static_cast<const TimeSeriesValueType &>(*signature.parameters[0].type);
    return {{std::nullopt, &time_series_set_type(key_type.value_type(), TimeSeriesGuarantee::ordinary)}};
}

const TimeSeriesType *SubscriptionService::implementation_output_type(const ResolvedSignature &signature) const {
    if (signature.output == nullptr) {
        return nullptr;
    }
    const auto &key_type = static_cast<const TimeSeriesValueType &>(*signature.parameters[0].type);
    return &time_series_dictionary_type(key_type.value_type(), *signature.output, TimeSeriesGuarantee::ordinary);
}

WiringPoint SubscriptionService::wire_implementation_requests(GraphWiring &wiring,
                                                              const ResolvedSignature &signature,
                                                              details::NestedBindingIdentity identity,
                                                              bool bind_clients) const {
    const auto &key_type = static_cast<const TimeSeriesValueType &>(*signature.parameters[0].type);
    const auto &request_type = time_series_dictionary_type(key_type.value_type(), TS<std::int64_t>::type(),
                                                          TimeSeriesGuarantee::ordinary);
    auto type_map = TypeMap{};
    type_map.set(details::TimeSeriesVariableInstance<SERVICE_REQUESTS>::get(), &request_type);
    const auto output = static_cast<WiringPoint>(service_request_provider(type_map));
    if (bind_clients) {
        wiring.provide_service_binding(identity, output);
    }

    const auto &type = static_cast<const TimeSeriesDictionaryType &>(output.type());
    return output.record()->wiring().output_item(output, key_set_item_id, type.key_set_type());
}

void SubscriptionService::append_implementation_request_arguments(WiringPoint requests,
                                                                  std::vector<WiringArgument> &arguments) const {
    arguments.emplace_back(requests);
}

std::optional<WiringPoint> SubscriptionService::wire_client(GraphWiring &wiring, ResolvedSignature signature,
                                                            const TypeMap &type_map,
                                                            std::span<const WiringArgument> arguments) const {
    const auto *key_type = type_cast<TimeSeriesValueType>(signature.parameters[0].type);
    if (key_type == nullptr || !key_type->value_type().supports_hash() ||
        !key_type->value_type().supports_equality()) {
        throw WiringError{"subscription service keys must support hashing and equality"};
    }

    auto &instance = wiring.root_service_requirements().require(*this, signature, type_map, arguments,
                                                                WiringContext::current().path());
    const auto identity = details::NestedBindingIdentity::object(&instance);
    const auto binding_index =
        wiring.require_service_binding(identity, GraphWiring::ServiceBindingOrdering::provider_after_consumers);
    const auto request_binding = ScalarValue<Opaque<std::size_t>>{binding_index};
    const auto request =
        std::array{WiringArgument{arguments[0].time_series()}, WiringArgument{request_binding}};
    const auto request_call = wiring.invoke_node(subscription_request_client, request);
    wiring.register_service_binding_consumer(identity, request_call.identity);

    const auto &response_type = *implementation_output_type(signature);
    const auto response = wiring.require_inherited_time_series(identity, signature.name, response_type);
    return operators::getitem(response, arguments[0].time_series());
}

details::ServiceInstance &details::ServiceRequirements::require(const ServiceDefinition &service,
                                                                ResolvedSignature signature,
                                                                const TypeMap &type_map,
                                                                std::span<const WiringArgument> arguments,
                                                                const WiringPath *call_path) {
    auto scalars = std::vector<ScalarValue<>>{};
    for (auto index = std::size_t{}; index < signature.parameters.size(); ++index) {
        const auto *scalar_type = type_cast<ScalarType>(signature.parameters[index].type);
        if (scalar_type == nullptr) {
            continue;
        }
        if (!scalar_type->supports_equality()) {
            throw WiringError{"service scalar parameters must support equality"};
        }

        scalars.emplace_back(arguments[index].scalar());
    }

    auto &instance = require_resolved(service, std::move(signature), type_map, std::move(scalars), call_path);
    instance.has_client = true;
    return instance;
}

details::ServiceInstance &details::ServiceRequirements::require_resolved(const ServiceDefinition &service,
                                                                         ResolvedSignature signature,
                                                                         const TypeMap &type_map,
                                                                         std::vector<ScalarValue<>> scalars,
                                                                         const WiringPath *call_path) {
    const auto call_context = WiringErrorContext{.stage = WiringFailureStage::requirement,
                                                .path = call_path == nullptr ? std::string{} : call_path->text(),
                                                .operation = signature.name};
    const auto active_contexts =
        details::WiringDiagnosticContext::current(WiringContext::current().wiring());
    auto call_contexts = std::vector<WiringErrorContext>{active_contexts.begin(), active_contexts.end()};
    if (std::ranges::find(call_contexts, call_context) == call_contexts.end()) {
        call_contexts.push_back(call_context);
    }

    const auto same_scalars = [&](const auto &instance) {
        if (instance.scalar_arguments.size() != scalars.size()) {
            return false;
        }
        for (auto index = std::size_t{}; index < scalars.size(); ++index) {
            if (&instance.scalar_arguments[index].type() != &scalars[index].type() ||
                !instance.scalar_arguments[index].equals(scalars[index])) {
                return false;
            }
        }
        return true;
    };

    const auto existing = std::ranges::find_if(instances_, [&](const auto &instance) {
        return instance->service == &service && same_resolved_signature(instance->signature, signature) &&
               same_scalars(*instance);
    });
    if (existing != instances_.end()) {
        auto &instance = **existing;
        for (const auto &context : call_contexts) {
            if (std::ranges::find(instance.call_contexts, context) == instance.call_contexts.end()) {
                instance.call_contexts.push_back(context);
            }
        }
        return instance;
    }

    auto instance =
        std::make_unique<ServiceInstance>(&service, std::move(signature), type_map.copy(), std::move(scalars), false,
                                          false, false, false);
    instance->call_contexts = std::move(call_contexts);
    auto &result = *instance;
    instances_.push_back(std::move(instance));
    return result;
}

details::ServiceRequirements &GraphWiring::root_service_requirements() {
    if (parent_ != nullptr) {
        return parent_->root_service_requirements();
    }
    if (!service_requirements_) {
        service_requirements_ = std::make_unique<details::ServiceRequirements>();
    }
    return *service_requirements_;
}

std::optional<WiringPoint> GraphWiring::require_service(const ServiceDefinition &service,
                                                        ResolvedSignature signature,
                                                        const TypeMap &type_map,
                                                        std::span<const WiringArgument> arguments) {
    auto &instance = root_service_requirements().require(service, std::move(signature), type_map, arguments,
                                                         WiringContext::current().path());
    if (instance.signature.output == nullptr) {
        return std::nullopt;
    }

    return require_inherited_time_series(details::NestedBindingIdentity::object(&instance), instance.signature.name,
                                         *instance.signature.output);
}

} // namespace bg

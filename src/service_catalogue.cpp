#include <bg/service_catalogue.hpp>

#include <algorithm>
#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/details/multiservice_ports.hpp>
#include <bg/graph/details/service_requirements.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <functional>
#include <exception>
#include <optional>
#include <ranges>
#include <string>
#include <vector>

namespace bg {

struct ServiceCatalogue::State {
    struct ScalarFilter {
        util::symbol name;
        ScalarValue<> value;
    };

    struct Registration {
        const ServiceDefinition *service;
        TypeMap type_map;
        std::shared_ptr<const WiringTemplate> implementation;
        std::vector<ScalarFilter> scalar_filters;
        std::vector<WiringArgument> installation_arguments;
        std::vector<std::size_t> lifted_parameters;
    };

    struct Claim {
        const ServiceDefinition *service;
        TypeMap type_map;
        std::optional<util::symbol> lifted_scalar;
        std::vector<ScalarFilter> scalar_filters;
    };

    struct MultiserviceRegistration {
        struct Reply {
            const ServiceDefinition *service;
            ResolvedSignature signature;
            TypeMap type_map;
            std::vector<ScalarValue<>> scalar_arguments;
            WiringPoint output;
        };

        std::shared_ptr<const WiringTemplate> implementation;
        std::vector<WiringArgument> installation_arguments;
        std::vector<Claim> claims;
    };

    struct MultiserviceBuildState {
        struct CatchAllPort {
            const ServiceDefinition *service;
            ResolvedSignature signature;
            TypeMap type_map;
            std::size_t lifted_parameter;
            std::vector<ScalarValue<>> unlifted_scalars;
            WiringPoint requests;
            bool requests_wired{};
            std::optional<WiringPoint> replies;
        };

        std::vector<MultiserviceRegistration::Reply> replies;
        std::vector<std::unique_ptr<CatchAllPort>> catch_all_ports;
        std::unique_ptr<details::MultiservicePorts> ports;
        bool wired{};
    };

    std::vector<Registration> registrations;
    std::vector<MultiserviceRegistration> multiservice_registrations;
};

namespace {

WiringError service_error(std::string message, const details::ServiceInstance &instance,
                          std::exception_ptr cause = {}) {
    return WiringError::from_exception(std::move(message), instance.call_contexts, std::move(cause));
}

template <typename Requests, typename Replies>
class MultiservicePortCallbacks final : public details::MultiservicePorts {
  public:
    MultiservicePortCallbacks(Requests requests, Replies replies)
        : requests_{std::move(requests)}, replies_{std::move(replies)} {}

    WiringPoint requests(const ServiceDefinition &service, const TypeMap &type_map,
                         std::span<const WiringArgument> scalar_arguments,
                         std::optional<util::symbol> lifted_scalar) override {
        return std::invoke(requests_, service, type_map, scalar_arguments, lifted_scalar);
    }

    void replies(const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
                 std::span<const WiringArgument> scalar_arguments,
                 std::optional<util::symbol> lifted_scalar) override {
        std::invoke(replies_, service, type_map, output, scalar_arguments, lifted_scalar);
    }

  private:
    Requests requests_;
    Replies replies_;
};

} // namespace

struct ServiceCatalogue::BuildOperation {
    struct Session {
        std::shared_ptr<const State> catalogue;
        std::vector<State::MultiserviceBuildState> multiservice_state;
        std::unique_ptr<BuildOperation> operation;
    };

    BuildOperation(const State &catalogue, GraphWiring &wiring, details::ServiceRequirements &requirements,
                   Session &session)
        : catalogue_{catalogue}, wiring_{wiring}, requirements_{requirements}, session_{session} {
        session_.multiservice_state.resize(catalogue.multiservice_registrations.size());
    }

    bool run();
    void validate() const;
    void wire_multiservice(std::size_t registration_index);
    void wire_catch_all(details::ServiceInstance &instance, const State::Registration &registration);

  private:
    struct ImplementationCandidates {
        std::vector<const State::Registration *> exact;
        std::vector<std::pair<std::size_t, const State::Claim *>> exact_multiservice;
        std::vector<const State::Registration *> catch_all;
        std::vector<std::pair<std::size_t, const State::Claim *>> catch_all_multiservice;
    };

    struct CatchAllGroup {
        const State::Registration *registration;
        const ServiceDefinition *service;
        ResolvedSignature signature;
        std::vector<ScalarValue<>> unlifted_scalars;
        std::optional<WiringPoint> replies;
    };

    static bool same_scalars(std::span<const ScalarValue<>> left, std::span<const ScalarValue<>> right);
    static bool matches_scalar_filters(std::span<const State::ScalarFilter> filters,
                                       std::span<const ResolvedParameter> parameters,
                                       std::span<const ScalarValue<>> scalars,
                                       std::optional<std::size_t> lifted_parameter = std::nullopt);
    ImplementationCandidates implementation_candidates(const details::ServiceInstance &instance) const;
    const State &catalogue_;
    GraphWiring &wiring_;
    details::ServiceRequirements &requirements_;
    Session &session_;
    std::vector<std::unique_ptr<CatchAllGroup>> catch_all_groups_;
};

ServiceCatalogue::ServiceCatalogue() : state_{std::make_shared<State>()} {}

ServiceCatalogue::~ServiceCatalogue() = default;

ServiceCatalogue::ServiceCatalogue(ServiceCatalogue &&) noexcept = default;

ServiceCatalogue &ServiceCatalogue::operator=(ServiceCatalogue &&) noexcept = default;

bool ServiceCatalogue::BuildOperation::same_scalars(std::span<const ScalarValue<>> left,
                                                    std::span<const ScalarValue<>> right) {
    if (left.size() != right.size()) {
        return false;
    }
    return std::ranges::equal(left, right, [](const auto &left_value, const auto &right_value) {
        return &left_value.type() == &right_value.type() && left_value.equals(right_value);
    });
}

bool ServiceCatalogue::BuildOperation::matches_scalar_filters(std::span<const State::ScalarFilter> filters,
                                                              std::span<const ResolvedParameter> parameters,
                                                              std::span<const ScalarValue<>> scalars,
                                                              std::optional<std::size_t> lifted_parameter) {
    for (const auto &filter : filters) {
        auto scalar_index = std::size_t{};
        auto found = false;
        for (auto parameter_index = std::size_t{}; parameter_index < parameters.size(); ++parameter_index) {
            const auto &parameter = parameters[parameter_index];
            if (!parameter.type->is_scalar()) {
                continue;
            }
            if (parameter.name == filter.name) {
                found = (!lifted_parameter || parameter_index != *lifted_parameter) &&
                        scalar_index < scalars.size() && scalars[scalar_index].equals(filter.value);
                break;
            }
            if (!lifted_parameter || parameter_index != *lifted_parameter) {
                ++scalar_index;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

ServiceCatalogue::BuildOperation::ImplementationCandidates
ServiceCatalogue::BuildOperation::implementation_candidates(const details::ServiceInstance &instance) const {
    auto candidates = ImplementationCandidates{};
    for (const auto &registration : catalogue_.registrations) {
        if (registration.service != instance.service || !instance.type_map.includes(registration.type_map) ||
            !matches_scalar_filters(registration.scalar_filters, instance.signature.parameters,
                                    instance.scalar_arguments)) {
            continue;
        }
        if (registration.lifted_parameters.empty()) {
            candidates.exact.push_back(&registration);
        } else {
            candidates.catch_all.push_back(&registration);
        }
    }

    for (auto index = std::size_t{}; index < catalogue_.multiservice_registrations.size(); ++index) {
        const auto &registration = catalogue_.multiservice_registrations[index];
        for (const auto &claim : registration.claims) {
            if (claim.service != instance.service || !instance.type_map.includes(claim.type_map) ||
                !matches_scalar_filters(claim.scalar_filters, instance.signature.parameters,
                                        instance.scalar_arguments)) {
                continue;
            }

            if (claim.lifted_scalar) {
                candidates.catch_all_multiservice.emplace_back(index, &claim);
            } else {
                candidates.exact_multiservice.emplace_back(index, &claim);
            }
        }
    }

    return candidates;
}

void ServiceCatalogue::add_impl(const ServiceDefinition &service, TypeMap type_map,
                                std::shared_ptr<const WiringTemplate> implementation,
                                std::span<const WiringArgument> arguments) {
    auto filters = std::vector<State::ScalarFilter>{};
    auto installation_arguments = std::vector<WiringArgument>{};
    filters.reserve(arguments.size());
    installation_arguments.reserve(arguments.size());
    for (const auto &argument : arguments) {
        if (argument.is_graph()) {
            throw std::invalid_argument{"a service implementation argument cannot be a graph"};
        }
        if (argument.is_scalar() && !argument.present()) {
            throw std::invalid_argument{"a service implementation scalar argument must have a value"};
        }

        if (argument.is_scalar() && argument.name()) {
            const auto parameter_indices = std::views::iota(std::size_t{}, service.signature().parameter_count());
            const auto parameter = std::ranges::find_if(parameter_indices, [&](std::size_t index) {
                const auto &candidate = service.signature().parameter(index);
                return candidate.name == *argument.name() && candidate.type->is_scalar();
            });
            if (parameter != parameter_indices.end()) {
                const auto &service_parameter = service.signature().parameter(*parameter);
                auto filter_type_map = TypeMap{};
                if (!service_parameter.type->match(argument.type(), filter_type_map)) {
                    throw std::invalid_argument{"a service scalar filter has an incompatible type"};
                }
                if (std::ranges::contains(filters, *argument.name(), &State::ScalarFilter::name)) {
                    throw std::invalid_argument{"a service implementation repeats a scalar filter"};
                }

                filters.push_back({*argument.name(), ScalarValue<>{argument.scalar()}});
                continue;
            }
        }

        installation_arguments.emplace_back(argument);
    }

    auto lifted_parameters = std::vector<std::size_t>{};
    if (dynamic_cast<const Service *>(&service) != nullptr) {
        const auto &implementation_signature = implementation->implementation_signature();
        for (auto parameter_index = std::size_t{}; parameter_index < service.signature().parameter_count();
             ++parameter_index) {
            const auto &parameter = service.signature().parameter(parameter_index);
            if (!parameter.type->is_scalar()) {
                continue;
            }
            const auto &implementation_parameters = implementation_signature;
            for (auto input_index = std::size_t{}; input_index < implementation_parameters.parameter_count();
                 ++input_index) {
                const auto &input = implementation_parameters.parameter(input_index);
                if (input.name != parameter.name) {
                    continue;
                }
                const auto *dictionary = type_cast<TimeSeriesDictionaryType>(input.type);
                const auto *value = dictionary == nullptr
                                        ? nullptr
                                        : type_cast<TimeSeriesValueType>(&dictionary->value_type());
                if (value != nullptr &&
                    dictionary->key_type().accepts(cpp_scalar_type<service_client_id_t>()) &&
                    value->guarantee().value() == TimeSeriesGuarantee::final) {
                    lifted_parameters.push_back(parameter_index);
                }
                break;
            }
        }
    }

    state_->registrations.push_back({&service, std::move(type_map), std::move(implementation), std::move(filters),
                                     std::move(installation_arguments), std::move(lifted_parameters)});
}

void ServiceCatalogue::register_impl(const ServiceDefinition &service,
                                     std::shared_ptr<const WiringTemplate> implementation,
                                     std::span<const WiringArgument> arguments) {
    add_impl(service, TypeMap{}, std::move(implementation), arguments);
}

std::size_t ServiceCatalogue::add_multiservice(std::shared_ptr<const WiringTemplate> implementation,
                                               std::span<const WiringArgument> arguments) {
    auto installation_arguments = std::vector<WiringArgument>{};
    installation_arguments.reserve(arguments.size());
    for (const auto &argument : arguments) {
        if (argument.is_graph()) {
            throw std::invalid_argument{"a multiservice implementation argument cannot be a graph"};
        }
        if (argument.is_scalar() && !argument.present()) {
            throw std::invalid_argument{"a multiservice implementation scalar argument must have a value"};
        }
        installation_arguments.emplace_back(argument);
    }

    auto &registrations = state_->multiservice_registrations;
    registrations.push_back({std::move(implementation), std::move(installation_arguments), {}});
    return registrations.size() - 1;
}

void ServiceCatalogue::add_multiservice_impl(const std::shared_ptr<State> &state, std::size_t index,
                                             const ServiceDefinition &service, TypeMap type_map,
                                             std::optional<util::symbol> lifted_scalar,
                                             std::span<const WiringArgument> filters) {
    if (index >= state->multiservice_registrations.size()) {
        throw std::out_of_range{"unknown multiservice registration"};
    }
    if (lifted_scalar && dynamic_cast<const Service *>(&service) == nullptr) {
        throw std::invalid_argument{"a multiservice catch-all requires a service implementation"};
    }

    if (lifted_scalar) {
        const auto lifted_parameter = std::ranges::find_if(
            std::views::iota(std::size_t{}, service.signature().parameter_count()), [&](std::size_t parameter_index) {
                const auto &parameter = service.signature().parameter(parameter_index);
                return parameter.name == *lifted_scalar && parameter.type->is_scalar();
            });
        if (lifted_parameter == std::views::iota(std::size_t{}, service.signature().parameter_count()).end()) {
            throw std::invalid_argument{"a multiservice catch-all must name a service scalar parameter"};
        }
    }

    auto scalar_filters = std::vector<State::ScalarFilter>{};
    scalar_filters.reserve(filters.size());
    for (const auto &filter : filters) {
        if (!filter.is_scalar() || !filter.present() || !filter.name()) {
            throw std::invalid_argument{"a multiservice scalar filter must be a named scalar value"};
        }

        const auto parameter = std::ranges::find_if(
            std::views::iota(std::size_t{}, service.signature().parameter_count()), [&](std::size_t parameter_index) {
                const auto &candidate = service.signature().parameter(parameter_index);
                return candidate.name == *filter.name() && candidate.type->is_scalar();
            });
        if (parameter == std::views::iota(std::size_t{}, service.signature().parameter_count()).end()) {
            throw std::invalid_argument{"a multiservice scalar filter must name a service scalar parameter"};
        }
        if (lifted_scalar && *filter.name() == *lifted_scalar) {
            throw std::invalid_argument{"a multiservice catch-all cannot filter its lifted scalar"};
        }
        if (std::ranges::contains(scalar_filters, *filter.name(), &State::ScalarFilter::name)) {
            throw std::invalid_argument{"a multiservice claim repeats a scalar filter"};
        }

        const auto &parameter_type = static_cast<const ScalarType &>(*service.signature().parameter(*parameter).type);
        auto type_map_check = TypeMap{};
        if (!parameter_type.match(filter.type(), type_map_check)) {
            throw std::invalid_argument{"a multiservice scalar filter has an incompatible type"};
        }

        scalar_filters.push_back({*filter.name(), ScalarValue<>{filter.scalar()}});
    }

    auto &claims = state->multiservice_registrations[index].claims;
    const auto duplicate = std::ranges::find_if(claims, [&](const auto &claim) {
        if (claim.service != &service || !claim.type_map.has_same_mappings(type_map) ||
            claim.lifted_scalar != lifted_scalar ||
            claim.scalar_filters.size() != scalar_filters.size()) {
            return false;
        }
        return std::ranges::all_of(scalar_filters, [&](const auto &filter) {
            const auto existing = std::ranges::find(claim.scalar_filters, filter.name, &State::ScalarFilter::name);
            return existing != claim.scalar_filters.end() && existing->value.equals(filter.value);
        });
    });
    if (duplicate != claims.end()) {
        throw std::invalid_argument{"a multiservice registration repeats a service claim"};
    }

    claims.push_back({&service, std::move(type_map), lifted_scalar, std::move(scalar_filters)});
}

void ServiceCatalogue::BuildOperation::wire_multiservice(std::size_t registration_index) {
    auto &registration = catalogue_.multiservice_registrations[registration_index];
    auto &build_state = session_.multiservice_state[registration_index];
    if (build_state.wired) {
        throw WiringError{"multiservice implementation was already wired but a later service requirement matched it"};
    }
    if (registration.claims.empty()) {
        throw WiringError{"a multiservice implementation has no registered service claims"};
    }

    struct ServiceCall {
        details::ServiceInstance *instance{};
        ResolvedSignature signature;
        TypeMap type_map;
        std::vector<ScalarValue<>> scalars;
        std::optional<std::size_t> lifted_parameter;
    };

    const auto copy_scalars = [](std::span<const ScalarValue<>> values) {
        auto result = std::vector<ScalarValue<>>{};
        result.reserve(values.size());
        for (const auto &value : values) {
            result.emplace_back(value.view());
        }
        return result;
    };

    const auto resolve_call = [this, copy_scalars](const ServiceDefinition &service, const TypeMap &seed,
                                                   std::span<const WiringArgument> arguments,
                                                   std::optional<util::symbol> lifted_scalar,
                                                   bool create_instance) {
        const auto &signature = service.signature();
        auto lifted_parameter = std::optional<std::size_t>{};
        if (lifted_scalar) {
            for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
                const auto &parameter = signature.parameter(index);
                if (parameter.name == *lifted_scalar && parameter.type->is_scalar()) {
                    lifted_parameter = index;
                    break;
                }
            }
            if (!lifted_parameter) {
                throw WiringError{"a multiservice catch-all port must name a service scalar parameter"};
            }
        }

        auto type_map = seed.copy();
        auto scalar_parameters = std::vector<std::size_t>{};
        for (auto index = std::size_t{}; index < signature.parameter_count(); ++index) {
            if (signature.parameter(index).type->is_scalar() && (!lifted_parameter || index != *lifted_parameter)) {
                scalar_parameters.push_back(index);
            }
        }
        if (arguments.size() != scalar_parameters.size()) {
            throw WiringError{"multiservice arguments must provide every unlifted service scalar parameter"};
        }

        auto scalar_values = std::vector<std::optional<ScalarValue<>>>(scalar_parameters.size());
        auto next_positional = std::size_t{};
        for (const auto &argument : arguments) {
            if (!argument.is_scalar() || !argument.present()) {
                throw WiringError{"multiservice arguments must be scalar values"};
            }

            auto scalar_position = std::size_t{};
            if (argument.name()) {
                const auto found = std::ranges::find_if(scalar_parameters, [&](std::size_t parameter) {
                    return signature.parameter(parameter).name == *argument.name();
                });
                if (found == scalar_parameters.end()) {
                    throw WiringError{
                        "a multiservice argument must name a service scalar parameter required by this call"};
                }
                scalar_position = static_cast<std::size_t>(found - scalar_parameters.begin());
            } else {
                while (next_positional < scalar_values.size() && scalar_values[next_positional]) {
                    ++next_positional;
                }
                if (next_positional == scalar_values.size()) {
                    throw WiringError{"multiservice arguments repeat a service scalar parameter"};
                }
                scalar_position = next_positional++;
            }
            if (scalar_values[scalar_position]) {
                throw WiringError{"multiservice arguments repeat a service scalar parameter"};
            }

            const auto parameter = scalar_parameters[scalar_position];
            if (!signature.parameter(parameter).type->match(argument.type(), type_map)) {
                throw WiringError{"a multiservice scalar argument has an incompatible type"};
            }
            scalar_values[scalar_position].emplace(argument.scalar());
        }

        auto scalars = std::vector<ScalarValue<>>{};
        scalars.reserve(scalar_values.size());
        for (auto &value : scalar_values) {
            if (!value) {
                throw WiringError{"multiservice arguments omit a required service scalar parameter"};
            }
            scalars.push_back(std::move(*value));
        }

        auto matching_instances = std::vector<details::ServiceInstance *>{};
        for (auto &candidate : requirements_.instances()) {
            if (candidate->service != &service || !candidate->type_map.includes(type_map)) {
                continue;
            }
            auto scalar_index = std::size_t{};
            auto argument_index = std::size_t{};
            auto matches = true;
            for (auto parameter_index = std::size_t{}; parameter_index < signature.parameter_count();
                 ++parameter_index) {
                if (!signature.parameter(parameter_index).type->is_scalar()) {
                    continue;
                }
                if (lifted_parameter && parameter_index == *lifted_parameter) {
                    ++scalar_index;
                    continue;
                }
                if (!candidate->scalar_arguments[scalar_index++].equals(scalars[argument_index++])) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                matching_instances.push_back(candidate.get());
            }
        }

        if (lifted_parameter && !matching_instances.empty()) {
            auto *first = matching_instances.front();
            for (const auto *candidate : matching_instances) {
                if (!same_resolved_signature(first->signature, candidate->signature) ||
                    !first->type_map.has_same_mappings(candidate->type_map)) {
                    throw WiringError{"multiservice catch-all arguments match multiple resolved service signatures"};
                }
            }
            type_map = first->type_map.copy();
            return ServiceCall{nullptr, first->signature, std::move(type_map), std::move(scalars), lifted_parameter};
        }

        if (!lifted_parameter && matching_instances.size() > 1) {
            throw WiringError{"multiservice arguments match more than one resolved service instance"};
        }
        if (!matching_instances.empty()) {
            auto *instance = matching_instances.front();
            type_map = instance->type_map.copy();
            scalars = copy_scalars(instance->scalar_arguments);
            return ServiceCall{instance, instance->signature, std::move(type_map), std::move(scalars), std::nullopt};
        }

        auto resolved_signature = signature.complete_resolution(type_map);
        if (lifted_parameter || !create_instance) {
            return ServiceCall{nullptr, std::move(resolved_signature), std::move(type_map), std::move(scalars),
                               lifted_parameter};
        }

        auto &instance = requirements_.require_resolved(service, resolved_signature, type_map, std::move(scalars),
                                                        WiringContext::current().path());
        return ServiceCall{&instance, std::move(resolved_signature), std::move(type_map),
                           copy_scalars(instance.scalar_arguments), std::nullopt};
    };

    const auto matching_claim = [this, registration_index](const ServiceDefinition &service,
                                                           const ServiceCall &call) -> const State::Claim & {
        auto matched = static_cast<const State::Claim *>(nullptr);
        for (const auto &claim : catalogue_.multiservice_registrations[registration_index].claims) {
            if (claim.service != &service || claim.lifted_scalar.has_value() != call.lifted_parameter.has_value() ||
                !call.type_map.includes(claim.type_map) ||
                !matches_scalar_filters(claim.scalar_filters, call.signature.parameters, call.scalars,
                                        call.lifted_parameter)) {
                continue;
            }
            if (call.lifted_parameter &&
                claim.lifted_scalar != call.signature.parameters[*call.lifted_parameter].name) {
                continue;
            }
            if (matched != nullptr) {
                if (!call.lifted_parameter) {
                    throw WiringError{"multiservice port matches more than one registered claim"};
                }
                // Catch-all claim ambiguity is checked for each client; exact clients do not use this port.
                continue;
            }
            matched = &claim;
        }

        if (matched == nullptr) {
            throw WiringError{call.lifted_parameter
                                  ? "a multiservice graph uses an unregistered catch-all service port"
                                  : "a multiservice graph uses an unregistered service port"};
        }
        return *matched;
    };

    const auto find_catch_all_port = [&session = session_, registration_index, copy_scalars](
                                         const ServiceDefinition &service, const ServiceCall &call,
                                         std::optional<WiringPoint> reply) {
        auto &build_state = session.multiservice_state[registration_index];
        const auto found = std::ranges::find_if(build_state.catch_all_ports, [&](const auto &port) {
            return port->service == &service && call.lifted_parameter &&
                   port->lifted_parameter == *call.lifted_parameter &&
                   same_resolved_signature(port->signature, call.signature) &&
                   port->type_map.has_same_mappings(call.type_map) &&
                   same_scalars(port->unlifted_scalars, call.scalars);
        });
        if (found != build_state.catch_all_ports.end()) {
            if (reply) {
                if ((*found)->replies) {
                    throw WiringError{"a multiservice catch-all reply was declared more than once"};
                }
                (*found)->replies = *reply;
            }
            return found->get();
        }

        auto port = std::make_unique<State::MultiserviceBuildState::CatchAllPort>();
        port->service = &service;
        port->signature = std::move(call.signature);
        port->type_map = call.type_map.copy();
        port->lifted_parameter = *call.lifted_parameter;
        port->unlifted_scalars = copy_scalars(call.scalars);
        port->replies = reply;
        auto *result = port.get();
        build_state.catch_all_ports.push_back(std::move(port));
        return result;
    };

    const auto bind_reply = [this](details::ServiceInstance &instance, WiringPoint output) {
        if (instance.implementation_replies_wired) {
            throw WiringError{"a multiservice service reply was declared more than once"};
        }
        const auto *expected_output = instance.service->implementation_output_type(instance.signature);
        if (expected_output == nullptr || !expected_output->accepts(output.type())) {
            throw WiringError{"a multiservice reply has an incompatible output time series"};
        }
        wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                              output);
        instance.implementation_replies_wired = true;
        instance.implementation_wired =
            !instance.service->implementation_requests_required() || instance.implementation_requests_wired;
    };

    const auto matching_reply = [this, registration_index](const details::ServiceInstance &instance) {
        const auto &replies = session_.multiservice_state[registration_index].replies;
        return std::ranges::find_if(replies, [&](const auto &reply) {
            if (reply.service != instance.service || !same_resolved_signature(reply.signature, instance.signature) ||
                !instance.type_map.includes(reply.type_map) ||
                !same_scalars(reply.scalar_arguments, instance.scalar_arguments)) {
                return false;
            }
            return true;
        });
    };

    auto callbacks = MultiservicePortCallbacks{
        [this, registration_index, resolve_call, matching_claim, find_catch_all_port, matching_reply, bind_reply](
            const ServiceDefinition &service, const TypeMap &type_map,
            std::span<const WiringArgument> scalar_arguments, std::optional<util::symbol> lifted_scalar) {
            auto &build_state = session_.multiservice_state[registration_index];
            if (lifted_scalar) {
                auto call = resolve_call(service, type_map, scalar_arguments, lifted_scalar, false);
                (void)matching_claim(service, call);
                auto *port = find_catch_all_port(service, std::move(call), std::nullopt);
                if (port->requests_wired) {
                    throw WiringError{"a multiservice catch-all request was declared more than once"};
                }

                auto &signature = port->signature;
                const auto &scalar_type = static_cast<const ScalarType &>(
                    *signature.parameters[port->lifted_parameter].type);
                const auto &value_type = time_series_value_type(scalar_type, TimeSeriesGuarantee::final);
                const auto &field_type = time_series_dictionary_type(
                    cpp_scalar_type<service_client_id_t>(), value_type, TimeSeriesGuarantee::ordinary);
                const auto field = SchemaField{signature.parameters[port->lifted_parameter].name, &field_type};
                const auto identity = details::NestedBindingIdentity::object(port);
                (void)wiring_.require_service_binding(
                    identity, GraphWiring::ServiceBindingOrdering::provider_after_consumers);
                auto context = WiringContext{wiring_};
                port->requests = static_cast<const Service &>(service).wire_catch_all_requests(
                    wiring_, signature, identity, std::span<const SchemaField>{&field, 1});
                port->requests_wired = true;
                return port->requests;
            }

            auto call = resolve_call(service, type_map, scalar_arguments, std::nullopt, true);
            (void)matching_claim(service, call);
            auto &instance = *call.instance;
            if (instance.implementation_requests_wired) {
                throw WiringError{"a multiservice service request was declared more than once"};
            }

            const auto requests = service.wire_implementation_requests(
                wiring_, instance.signature, details::NestedBindingIdentity::object(&instance), instance.has_client);

            instance.implementation_requests_wired = true;
            if (instance.signature.output == nullptr) {
                instance.implementation_wired = true;
            } else if (const auto reply = matching_reply(instance); reply != build_state.replies.end()) {
                bind_reply(instance, reply->output);
            }

            return requests;
        },
        [this, registration_index, resolve_call, matching_claim, find_catch_all_port, bind_reply, copy_scalars](
            const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
            std::span<const WiringArgument> scalar_arguments, std::optional<util::symbol> lifted_scalar) {
            auto &build_state = session_.multiservice_state[registration_index];
            if (lifted_scalar) {
                auto call = resolve_call(service, type_map, scalar_arguments, lifted_scalar, false);
                (void)matching_claim(service, call);
                const auto *expected_output = service.implementation_output_type(call.signature);
                if (expected_output == nullptr || !output || !expected_output->accepts(output.type())) {
                    throw WiringError{"a multiservice catch-all reply has an incompatible output time series"};
                }

                (void)find_catch_all_port(service, std::move(call), output);
                return;
            }

            auto call = resolve_call(service, type_map, scalar_arguments, std::nullopt, false);
            (void)matching_claim(service, call);
            auto reply_instance = details::ServiceInstance{
                &service, call.signature, call.type_map.copy(), copy_scalars(call.scalars), false, false, false, false};
            const auto *expected_output = service.implementation_output_type(reply_instance.signature);
            if (expected_output == nullptr || !output || !expected_output->accepts(output.type())) {
                throw WiringError{"a multiservice reply has an incompatible output time series"};
            }

            const auto duplicate = std::ranges::find_if(build_state.replies, [&](const auto &reply) {
                if (reply.service != &service || !same_resolved_signature(reply.signature, call.signature) ||
                    !reply.type_map.has_same_mappings(call.type_map) ||
                    reply.scalar_arguments.size() != call.scalars.size()) {
                    return false;
                }
                return std::ranges::equal(reply.scalar_arguments, call.scalars,
                                          [](const auto &left, const auto &right) {
                                              return &left.type() == &right.type() && left.equals(right);
                                          });
            });
            if (duplicate != build_state.replies.end()) {
                throw WiringError{"a multiservice service reply was declared more than once"};
            }

            build_state.replies.push_back(
                {&service, call.signature, call.type_map.copy(), copy_scalars(call.scalars), output});
            if (call.instance == nullptr) {
                return;
            }
            auto &instance = *call.instance;
            bind_reply(instance, output);
        }};
    build_state.ports = std::make_unique<decltype(callbacks)>(std::move(callbacks));

    auto wiring_context = WiringContext{wiring_, *build_state.ports};
    auto service_contexts = std::vector<WiringErrorContext>{};
    for (const auto &instance_owner : requirements_.instances()) {
        const auto &instance = *instance_owner;
        const auto candidates = implementation_candidates(instance);
        const auto exact_count = candidates.exact.size() + candidates.exact_multiservice.size();
        const auto catch_all_count = candidates.catch_all.size() + candidates.catch_all_multiservice.size();
        const auto selected = exact_count != 0
                                  ? std::ranges::any_of(candidates.exact_multiservice, [&](const auto &candidate) {
                                        return candidate.first == registration_index;
                                    })
                                  : catch_all_count == 1 &&
                                        !candidates.catch_all_multiservice.empty() &&
                                        candidates.catch_all_multiservice.front().first == registration_index;
        if (selected) {
            service_contexts.insert(service_contexts.end(), instance.call_contexts.begin(),
                                    instance.call_contexts.end());
        }
    }
    auto diagnostics = details::WiringDiagnosticContext{wiring_, service_contexts};
    auto output = std::optional<WiringPoint>{};
    try {
        output = (*registration.implementation)(std::span<const WiringArgument>{registration.installation_arguments});
    } catch (...) {
        throw WiringError::from_exception("multiservice implementation wiring failed", service_contexts,
                                          std::current_exception());
    }
    if (output) {
        throw WiringError::from_exception(
            "a multiservice graph must connect service outputs through implementation_replies", service_contexts, {});
    }

    const auto same_unlifted_scalars = [](const details::ServiceInstance &instance,
                                          const State::MultiserviceBuildState::CatchAllPort &port) {
        auto scalar_index = std::size_t{};
        auto unlifted_index = std::size_t{};
        for (auto parameter_index = std::size_t{}; parameter_index < instance.signature.parameters.size();
             ++parameter_index) {
            if (!instance.signature.parameters[parameter_index].type->is_scalar()) {
                continue;
            }
            if (parameter_index != port.lifted_parameter) {
                if (!instance.scalar_arguments[scalar_index].equals(port.unlifted_scalars[unlifted_index++])) {
                    return false;
                }
            }
            ++scalar_index;
        }
        return true;
    };

    for (const auto &port_owner : build_state.catch_all_ports) {
        auto &port = *port_owner;
        const auto lifted_name = port.signature.parameters[port.lifted_parameter].name;
        const auto &service = static_cast<const Service &>(*port.service);
        for (const auto &instance_owner : requirements_.instances()) {
            auto &instance = *instance_owner;
            if (instance.service != port.service ||
                !same_resolved_signature(instance.signature, port.signature) ||
                !instance.type_map.has_same_mappings(port.type_map) || !same_unlifted_scalars(instance, port)) {
                continue;
            }

            const auto candidates = implementation_candidates(instance);
            const auto exact_count = candidates.exact.size() + candidates.exact_multiservice.size();
            const auto catch_all_count = candidates.catch_all.size() + candidates.catch_all_multiservice.size();
            if (exact_count != 0) {
                continue;
            }
            if (catch_all_count > 1) {
                throw service_error("service '" + std::string{instance.signature.name.text()} +
                                        "' has ambiguous catch-all implementations",
                                    instance);
            }
            if (catch_all_count == 0 || candidates.catch_all_multiservice.empty() ||
                candidates.catch_all_multiservice.front().first != registration_index ||
                candidates.catch_all_multiservice.front().second->lifted_scalar != lifted_name) {
                continue;
            }

            if (port.requests_wired && !instance.implementation_requests_wired) {
                auto scalar_index = std::size_t{};
                for (auto parameter_index = std::size_t{}; parameter_index < instance.signature.parameters.size();
                     ++parameter_index) {
                    if (!instance.signature.parameters[parameter_index].type->is_scalar()) {
                        continue;
                    }
                    if (parameter_index == port.lifted_parameter) {
                        auto lifted = WiringArgument{ScalarValue<>{instance.scalar_arguments[scalar_index].view()}};
                        lifted.set_name(lifted_name);
                        lifted.match_fixed_parameter_by_name();
                        auto context = WiringContext{wiring_};
                        service.wire_forwarding_requests(
                            wiring_, details::NestedBindingIdentity::object(&instance),
                            details::NestedBindingIdentity::object(&port),
                            std::span<const WiringArgument>{&lifted, 1});
                        instance.implementation_requests_wired = true;
                        break;
                    }
                    ++scalar_index;
                }
            }

            if (port.replies && !instance.implementation_replies_wired) {
                const auto *expected_output = service.implementation_output_type(instance.signature);
                if (expected_output == nullptr || !expected_output->accepts(port.replies->type())) {
                    throw service_error("a multiservice catch-all reply has an incompatible output", instance);
                }
                wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance),
                                                      *expected_output, *port.replies);
                instance.implementation_replies_wired = true;
            }
            instance.implementation_wired = instance.implementation_requests_wired &&
                                            (instance.signature.output == nullptr ||
                                             instance.implementation_replies_wired);
        }
    }

    build_state.wired = true;
}

void ServiceCatalogue::BuildOperation::wire_catch_all(details::ServiceInstance &instance,
                                                      const State::Registration &registration) {
    auto unlifted_scalars = std::vector<ScalarValue<>>{};
    auto lifted_scalars = std::vector<WiringArgument>{};
    auto lifted_fields = std::vector<SchemaField>{};
    auto scalar_index = std::size_t{};
    for (auto parameter_index = std::size_t{}; parameter_index < instance.signature.parameters.size();
         ++parameter_index) {
        const auto &parameter = instance.signature.parameters[parameter_index];
        if (!parameter.type->is_scalar()) {
            continue;
        }
        const auto &scalar = instance.scalar_arguments[scalar_index++];
        if (!std::ranges::contains(registration.lifted_parameters, parameter_index)) {
            unlifted_scalars.emplace_back(scalar.view());
            continue;
        }

        auto argument = WiringArgument{ScalarValue<>{scalar.view()}};
        argument.set_name(parameter.name);
        argument.match_fixed_parameter_by_name();
        lifted_scalars.push_back(std::move(argument));
        const auto &scalar_type = static_cast<const ScalarType &>(*parameter.type);
        const auto &value_type = time_series_value_type(scalar_type, TimeSeriesGuarantee::final);
        lifted_fields.push_back({parameter.name,
                                 &time_series_dictionary_type(cpp_scalar_type<service_client_id_t>(), value_type,
                                                              TimeSeriesGuarantee::ordinary)});
    }

    auto found = std::ranges::find_if(catch_all_groups_, [&](const auto &group) {
        return group->registration == &registration && group->service == instance.service &&
               same_resolved_signature(group->signature, instance.signature) &&
               same_scalars(group->unlifted_scalars, unlifted_scalars);
    });
    if (found == catch_all_groups_.end()) {
        auto group = std::make_unique<CatchAllGroup>();
        group->registration = &registration;
        group->service = instance.service;
        group->signature = instance.signature;
        group->unlifted_scalars = std::move(unlifted_scalars);
        auto &created = *group;
        catch_all_groups_.push_back(std::move(group));

        auto diagnostics = details::WiringDiagnosticContext{wiring_, instance.call_contexts};
        auto context = WiringContext{wiring_};
        const auto identity = details::NestedBindingIdentity::object(&created);
        (void)wiring_.require_service_binding(identity, GraphWiring::ServiceBindingOrdering::provider_after_consumers);
        const auto &service = static_cast<const Service &>(*instance.service);
        const auto requests = service.wire_catch_all_requests(wiring_, instance.signature, identity, lifted_fields);
        auto arguments = std::vector<WiringArgument>{};
        service.append_implementation_request_arguments(requests, arguments);
        for (const auto &scalar : created.unlifted_scalars) {
            arguments.emplace_back(ScalarValue<>{scalar.view()});
        }
        for (const auto &argument : registration.installation_arguments) {
            arguments.emplace_back(argument);
        }

        try {
            const auto bound = details::bind_wiring_arguments(registration.implementation->signature(), arguments);
            created.replies = (*registration.implementation)(instance.type_map,
                                                              std::span<const WiringArgument>{bound});
        } catch (...) {
            throw service_error("catch-all service implementation wiring failed for '" +
                                    std::string{instance.signature.name.text()} + "'",
                                instance, std::current_exception());
        }

        const auto *expected = instance.service->implementation_output_type(instance.signature);
        if (expected == nullptr ? created.replies.has_value()
                                : !created.replies || !expected->accepts(created.replies->type())) {
            throw service_error("catch-all service implementation has an incompatible output", instance);
        }
        found = catch_all_groups_.end() - 1;
    }

    auto &group = **found;
    auto context = WiringContext{wiring_};
    static_cast<const Service &>(*instance.service)
        .wire_forwarding_requests(wiring_, details::NestedBindingIdentity::object(&instance),
                                  details::NestedBindingIdentity::object(&group), lifted_scalars);
    if (group.replies) {
        const auto &expected = *instance.service->implementation_output_type(instance.signature);
        wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), expected,
                                              *group.replies);
    }
    instance.implementation_wired = true;
}

bool ServiceCatalogue::BuildOperation::run() {
    auto changed = false;
    const auto apply_replies = [&](details::ServiceInstance &instance) {
        if (instance.implementation_replies_wired) {
            return false;
        }
        const State::MultiserviceRegistration::Reply *matching_reply{};
        for (const auto registration_index :
             std::views::iota(std::size_t{}, catalogue_.multiservice_registrations.size())) {
            for (const auto &reply : session_.multiservice_state[registration_index].replies) {
                if (reply.service != instance.service ||
                    !same_resolved_signature(reply.signature, instance.signature) ||
                    !instance.type_map.includes(reply.type_map) ||
                    !same_scalars(reply.scalar_arguments, instance.scalar_arguments)) {
                    continue;
                }
                if (matching_reply != nullptr) {
                    throw service_error("multiple multiservice replies match service '" +
                                            std::string{instance.signature.name.text()} + "'",
                                        instance);
                }
                matching_reply = &reply;
            }
        }
        if (matching_reply == nullptr) {
            return false;
        }

        const auto *expected_output = instance.service->implementation_output_type(instance.signature);
        if (expected_output == nullptr || !expected_output->accepts(matching_reply->output.type())) {
            throw service_error("a multiservice reply no longer matches service '" +
                                    std::string{instance.signature.name.text()} + "'",
                                instance);
        }
        wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                              matching_reply->output);
        instance.implementation_replies_wired = true;
        instance.implementation_wired =
            !instance.service->implementation_requests_required() || instance.implementation_requests_wired;
        return true;
    };

    for (auto instance_index = std::size_t{}; instance_index < requirements_.instances().size(); ++instance_index) {
        auto &instance = *requirements_.instances()[instance_index];
        changed = apply_replies(instance) || changed;
        if (!instance.has_client && !instance.implementation_replies_wired) {
            continue;
        }

        const auto candidates = implementation_candidates(instance);
        const auto service_name = std::string{instance.signature.name.text()};
        const auto exact_count = candidates.exact.size() + candidates.exact_multiservice.size();
        const auto catch_all_count = candidates.catch_all.size() + candidates.catch_all_multiservice.size();
        const auto candidate_count = exact_count == 0 ? catch_all_count : exact_count;
        if (candidate_count == 0) {
            throw service_error("service '" + service_name + "' has no matching implementation", instance);
        }
        if (candidate_count != 1) {
            throw service_error("service '" + service_name + "' has ambiguous implementations", instance);
        }
        if (instance.implementation_wired) {
            continue;
        }

        if (exact_count == 0) {
            if (!candidates.catch_all.empty()) {
                wire_catch_all(instance, *candidates.catch_all.front());
                changed = true;
                continue;
            }
            const auto index = candidates.catch_all_multiservice.front().first;
            if (session_.multiservice_state[index].wired) {
                continue;
            }

            wire_multiservice(index);
            changed = true;
            continue;
        }

        if (!candidates.exact_multiservice.empty()) {
            const auto index = candidates.exact_multiservice.front().first;
            if (session_.multiservice_state[index].wired) {
                continue;
            }

            wire_multiservice(index);
            changed = true;
            continue;
        }

        auto diagnostics = details::WiringDiagnosticContext{wiring_, instance.call_contexts};
        auto context = WiringContext{wiring_};
        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(instance.scalar_arguments.size() + instance.signature.parameters.size() +
                          candidates.exact.front()->installation_arguments.size());
        if (instance.service->implementation_requests_required()) {
            const auto requests = instance.service->wire_implementation_requests(
                wiring_, instance.signature, details::NestedBindingIdentity::object(&instance), true);
            instance.service->append_implementation_request_arguments(requests, arguments);
        }
        for (const auto &scalar : instance.scalar_arguments) {
            arguments.emplace_back(ScalarValue<>{scalar.view()});
        }
        for (const auto &argument : candidates.exact.front()->installation_arguments) {
            arguments.emplace_back(argument);
        }

        auto output = std::optional<WiringPoint>{};
        try {
            const auto bound = details::bind_wiring_arguments(candidates.exact.front()->implementation->signature(),
                                                               arguments);
            output = (*candidates.exact.front()->implementation)(instance.type_map,
                                                           std::span<const WiringArgument>{bound});
        } catch (...) {
            throw service_error("service implementation wiring failed for '" + service_name + "'", instance,
                                std::current_exception());
        }
        const auto *expected_output = instance.service->implementation_output_type(instance.signature);

        if (expected_output == nullptr) {
            if (output) {
                throw service_error("service '" + service_name + "' implementation produced unexpected output",
                                    instance);
            }
        } else {
            if (!output || !expected_output->accepts(output->type())) {
                auto message = "service '" + service_name + "' implementation produced an incompatible output";
                if (output) {
                    message += "\n  expected: ";
                    expected_output->format_name(message);
                    message += "\n  received: ";
                    output->type().format_name(message);
                } else {
                    message += "\n  expected: ";
                    expected_output->format_name(message);
                    message += "\n  received no output";
                }
                auto error = service_error(std::move(message), instance);
                if (output) {
                    auto actual = std::string{};
                    auto expected = std::string{};
                    output->type().format_name(actual);
                    expected_output->format_name(expected);
                    error = error.with_context({.stage = WiringFailureStage::type_match,
                                                .operation = instance.signature.name,
                                                .expected_type = std::move(expected),
                                                .actual_type = std::move(actual)});
                }
                throw error;
            }
            wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                                  *output);
        }

        instance.implementation_wired = true;
        changed = true;
    }

    return changed;
}

void ServiceCatalogue::BuildOperation::validate() const {
    for (auto registration_index = std::size_t{};
         registration_index < catalogue_.multiservice_registrations.size(); ++registration_index) {
        const auto &registration = catalogue_.multiservice_registrations[registration_index];
        const auto &build_state = session_.multiservice_state[registration_index];
        if (!build_state.wired) {
            continue;
        }

        for (const auto &instance_owner : requirements_.instances()) {
            const auto &instance = *instance_owner;
            const auto candidates = implementation_candidates(instance);
            const auto exact_count = candidates.exact.size() + candidates.exact_multiservice.size();
            const auto &claims = exact_count == 0 ? candidates.catch_all_multiservice : candidates.exact_multiservice;
            const auto claimed = std::ranges::any_of(claims, [&](const auto &candidate) {
                return candidate.first == registration_index;
            });
            if (!claimed) {
                continue;
            }

            const auto service_name = std::string{instance.signature.name.text()};
            if (instance.service->implementation_requests_required() && !instance.implementation_requests_wired) {
                throw service_error("multiservice implementation is missing implementation_requests for service '" +
                                        service_name + "'",
                                    instance);
            }
            if (instance.signature.output != nullptr && !instance.implementation_replies_wired) {
                throw service_error("multiservice implementation is missing implementation_replies for service '" +
                                        service_name + "'",
                                    instance);
            }
        }
    }
}

void ServiceCatalogue::build(GraphWiring &wiring) const {
    if (!wiring.is_root()) {
        throw std::invalid_argument{"services can only be built on the root GraphWiring"};
    }

    auto session = std::static_pointer_cast<BuildOperation::Session>(wiring.service_build_session(state_.get()));
    if (!session) {
        session = std::make_shared<BuildOperation::Session>();
        session->catalogue = state_;
        session->multiservice_state.resize(state_->multiservice_registrations.size());
        auto build = [&wiring, session] {
            auto *requirements = wiring.service_requirements();
            if (requirements == nullptr) {
                return false;
            }
            if (!session->operation) {
                session->operation =
                    std::make_unique<BuildOperation>(*session->catalogue, wiring, *requirements, *session);
            }
            return session->operation->run();
        };
        auto validate = [session] {
            if (session->operation) {
                session->operation->validate();
            }
        };
        wiring.attach_service_build_session(state_.get(), session, std::move(build), std::move(validate));
    }

    (void)wiring.complete_pending_wiring();
    while (true) {
        const auto services_changed = wiring.build_known_services();
        const auto wiring_changed = wiring.complete_pending_wiring();
        if (!services_changed && !wiring_changed) {
            return;
        }
    }
}

} // namespace bg

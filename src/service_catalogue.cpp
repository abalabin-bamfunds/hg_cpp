#include <bg/service_catalogue.hpp>

#include <algorithm>
#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/details/service_implementation.hpp>
#include <bg/graph/details/service_requirements.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/type_text.hpp>
#include <functional>
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
    };

    struct Claim {
        const ServiceDefinition *service;
        TypeMap type_map;
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
        std::vector<MultiserviceRegistration::Reply> replies;
        bool wired{};
    };

    std::vector<Registration> registrations;
    std::vector<MultiserviceRegistration> multiservice_registrations;
};

namespace {

template <typename Requests, typename Replies>
class MultiservicePorts final : public details::ServiceImplementationAccess {
  public:
    MultiservicePorts(Requests requests, Replies replies)
        : requests_{std::move(requests)}, replies_{std::move(replies)} {}

    WiringPoint requests(const ServiceDefinition &service, const TypeMap &type_map,
                         std::span<const WiringArgument> scalar_arguments) override {
        return std::invoke(requests_, service, type_map, scalar_arguments);
    }

    void replies(const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
                 std::span<const WiringArgument> scalar_arguments) override {
        std::invoke(replies_, service, type_map, output, scalar_arguments);
    }

  private:
    Requests requests_;
    Replies replies_;
};

} // namespace

struct ServiceCatalogue::BuildOperation {
    BuildOperation(const State &catalogue, GraphWiring &wiring, details::ServiceRequirements &requirements)
        : catalogue_{catalogue}, wiring_{wiring}, requirements_{requirements},
          multiservice_state_(catalogue.multiservice_registrations.size()) {}

    void run();
    void wire_multiservice(std::size_t registration_index, details::ServiceInstance &required_instance);

  private:
    static bool same_scalars(std::span<const ScalarValue<>> left, std::span<const ScalarValue<>> right);
    static bool matches_scalar_filters(std::span<const State::ScalarFilter> filters,
                                       std::span<const ResolvedParameter> parameters,
                                       std::span<const ScalarValue<>> scalars);
    const State &catalogue_;
    GraphWiring &wiring_;
    details::ServiceRequirements &requirements_;
    std::vector<State::MultiserviceBuildState> multiservice_state_;
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
                                                              std::span<const ScalarValue<>> scalars) {
    for (const auto &filter : filters) {
        auto scalar_index = std::size_t{};
        auto found = false;
        for (const auto &parameter : parameters) {
            if (!parameter.type->is_scalar()) {
                continue;
            }
            if (parameter.name == filter.name) {
                found = scalar_index < scalars.size() && scalars[scalar_index].equals(filter.value);
                break;
            }
            ++scalar_index;
        }
        if (!found) {
            return false;
        }
    }
    return true;
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

    state_->registrations.push_back({&service, std::move(type_map), std::move(implementation), std::move(filters),
                                     std::move(installation_arguments)});
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
                                             std::span<const WiringArgument> filters) {
    if (index >= state->multiservice_registrations.size()) {
        throw std::out_of_range{"unknown multiservice registration"};
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

    claims.push_back({&service, std::move(type_map), std::move(scalar_filters)});
}

void ServiceCatalogue::BuildOperation::wire_multiservice(std::size_t registration_index,
                                                         details::ServiceInstance &required_instance) {
    auto &wiring = wiring_;
    auto &registration = catalogue_.multiservice_registrations[registration_index];
    auto &build_state = multiservice_state_[registration_index];
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
    };

    const auto copy_scalars = [](std::span<const ScalarValue<>> values) {
        auto result = std::vector<ScalarValue<>>{};
        result.reserve(values.size());
        for (const auto &value : values) {
            result.emplace_back(value.view());
        }
        return result;
    };

    const auto resolve_call = [&](const ServiceDefinition &service, const TypeMap &seed,
                                  std::span<const WiringArgument> arguments, bool create_instance) {
        auto type_map = seed.copy();
        auto scalar_parameters = std::vector<std::size_t>{};
        for (auto index = std::size_t{}; index < service.signature().parameter_count(); ++index) {
            if (service.signature().parameter(index).type->is_scalar()) {
                scalar_parameters.push_back(index);
            }
        }
        if (arguments.size() != scalar_parameters.size()) {
            throw WiringError{"multiservice service arguments must provide every service scalar parameter"};
        }

        auto scalar_values = std::vector<std::optional<ScalarValue<>>>(scalar_parameters.size());
        auto next_positional = std::size_t{};
        for (const auto &argument : arguments) {
            if (!argument.is_scalar() || !argument.present()) {
                throw WiringError{"multiservice service arguments must be scalar values"};
            }

            auto scalar_position = std::size_t{};
            if (argument.name()) {
                const auto found = std::ranges::find_if(scalar_parameters, [&](std::size_t parameter) {
                    return service.signature().parameter(parameter).name == *argument.name();
                });
                if (found == scalar_parameters.end()) {
                    throw WiringError{"multiservice service argument names a non-scalar service parameter"};
                }
                scalar_position = static_cast<std::size_t>(found - scalar_parameters.begin());
            } else {
                while (next_positional < scalar_values.size() && scalar_values[next_positional]) {
                    ++next_positional;
                }
                if (next_positional == scalar_values.size()) {
                    throw WiringError{"multiservice service arguments repeat a scalar parameter"};
                }
                scalar_position = next_positional++;
            }
            if (scalar_values[scalar_position]) {
                throw WiringError{"multiservice service arguments repeat a scalar parameter"};
            }

            const auto parameter = scalar_parameters[scalar_position];
            if (!service.signature().parameter(parameter).type->match(argument.type(), type_map)) {
                throw WiringError{"a multiservice scalar argument has an incompatible type"};
            }
            scalar_values[scalar_position].emplace(argument.scalar());
        }

        auto scalars = std::vector<ScalarValue<>>{};
        scalars.reserve(scalar_values.size());
        for (auto &value : scalar_values) {
            if (!value) {
                throw WiringError{"multiservice service arguments omit a scalar parameter"};
            }
            scalars.push_back(std::move(*value));
        }

        auto matching_instances = std::vector<details::ServiceInstance *>{};
        for (auto &candidate : requirements_.instances()) {
            if (candidate->service == &service && same_scalars(candidate->scalar_arguments, scalars) &&
                candidate->type_map.includes(type_map)) {
                matching_instances.push_back(candidate.get());
            }
        }
        if (matching_instances.size() > 1) {
            throw WiringError{"multiservice arguments match more than one resolved service instance"};
        }

        if (!matching_instances.empty()) {
            auto *instance = matching_instances.front();
            type_map = instance->type_map.copy();
            scalars = copy_scalars(instance->scalar_arguments);
            return ServiceCall{instance, instance->signature, std::move(type_map), std::move(scalars)};
        }

        auto resolved_signature = service.signature().complete_resolution(type_map);
        if (!create_instance) {
            return ServiceCall{nullptr, std::move(resolved_signature), std::move(type_map), std::move(scalars)};
        }

        auto &instance = requirements_.require_resolved(service, resolved_signature, type_map, std::move(scalars),
                                                        WiringContext::current().path());
        return ServiceCall{&instance, std::move(resolved_signature), std::move(type_map), std::move(scalars)};
    };

    const auto matching_claim = [&](const ServiceDefinition &service, const TypeMap &type_map,
                                    const ResolvedSignature &signature,
                                    std::span<const ScalarValue<>> scalars) -> const State::Claim & {
        auto matched = static_cast<const State::Claim *>(nullptr);
        for (const auto &claim : registration.claims) {
            if (claim.service != &service || !type_map.includes(claim.type_map)) {
                continue;
            }

            if (!matches_scalar_filters(claim.scalar_filters, signature.parameters, scalars)) {
                continue;
            }
            if (matched != nullptr) {
                throw WiringError{"multiservice port matches more than one registered claim"};
            }
            matched = &claim;
        }

        if (matched == nullptr) {
            throw WiringError{"a multiservice graph uses an unregistered service port"};
        }
        return *matched;
    };

    const auto bind_reply = [&](details::ServiceInstance &instance, WiringPoint output) {
        if (instance.implementation_replies_wired) {
            throw WiringError{"a multiservice service reply was declared more than once"};
        }
        const auto *expected_output = instance.service->implementation_output_type(instance.signature);
        if (expected_output == nullptr || !expected_output->accepts(output.type())) {
            throw WiringError{"a multiservice reply has an incompatible output time series"};
        }
        wiring.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                             output);
        instance.implementation_replies_wired = true;
        instance.implementation_wired =
            !instance.service->implementation_requests_required() || instance.implementation_requests_wired;
    };

    const auto matching_reply = [&](const details::ServiceInstance &instance) {
        return std::ranges::find_if(build_state.replies, [&](const auto &reply) {
            if (reply.service != instance.service || !same_resolved_signature(reply.signature, instance.signature) ||
                !instance.type_map.includes(reply.type_map) ||
                !same_scalars(reply.scalar_arguments, instance.scalar_arguments)) {
                return false;
            }
            return true;
        });
    };

    auto request_instances = std::vector<details::ServiceInstance *>{};
    auto reply_instances = std::vector<details::ServiceInstance *>{};
    auto service_access = MultiservicePorts{
        [&](const ServiceDefinition &service, const TypeMap &type_map,
            std::span<const WiringArgument> scalar_arguments) {
            auto call = resolve_call(service, type_map, scalar_arguments, true);
            (void)matching_claim(service, call.type_map, call.signature, call.scalars);
            auto &instance = *call.instance;
            if (instance.implementation_requests_wired) {
                throw WiringError{"a multiservice service request was declared more than once"};
            }

            const auto requests = service.wire_implementation_requests(
                wiring, instance.signature, details::NestedBindingIdentity::object(&instance), instance.has_client);

            instance.implementation_requests_wired = true;
            request_instances.push_back(&instance);
            if (instance.signature.output == nullptr) {
                instance.implementation_wired = true;
            } else if (const auto reply = matching_reply(instance); reply != build_state.replies.end()) {
                bind_reply(instance, reply->output);
                if (std::ranges::find(reply_instances, &instance) == reply_instances.end()) {
                    reply_instances.push_back(&instance);
                }
            }

            return requests;
        },
        [&](const ServiceDefinition &service, const TypeMap &type_map, WiringPoint output,
            std::span<const WiringArgument> scalar_arguments) {
            auto call = resolve_call(service, type_map, scalar_arguments, false);
            (void)matching_claim(service, call.type_map, call.signature, call.scalars);
            auto reply_instance = details::ServiceInstance{
                &service, call.signature, call.type_map.copy(), copy_scalars(call.scalars), false, false, false, false,
                {}};
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
            reply_instances.push_back(&instance);
        }};

    auto wiring_context = WiringContext{wiring, service_access};
    const auto output =
        (*registration.implementation)(std::span<const WiringArgument>{registration.installation_arguments});
    if (output) {
        throw WiringError{"a multiservice graph must connect service outputs through implementation_replies"};
    }

    build_state.wired = true;
    if (required_instance.service->implementation_requests_required() &&
        std::ranges::find(request_instances, &required_instance) == request_instances.end()) {
        throw WiringError{"multiservice implementation is missing implementation_requests for service '" +
                          std::string{required_instance.signature.name.text()} + "'"};
    }
    if (required_instance.signature.output != nullptr &&
        std::ranges::find(reply_instances, &required_instance) == reply_instances.end()) {
        throw WiringError{"multiservice implementation is missing implementation_replies for service '" +
                          std::string{required_instance.signature.name.text()} + "'"};
    }
}

void ServiceCatalogue::BuildOperation::run() {
    const auto service_paths = [](const details::ServiceInstance &instance) {
        auto result = std::string{};
        for (const auto *path : instance.call_paths) {
            if (path == nullptr) {
                continue;
            }
            result += "\n  at ";
            result += path->text();
        }
        return result;
    };

    const auto apply_replies = [&](details::ServiceInstance &instance) {
        if (instance.implementation_replies_wired) {
            return;
        }
        const State::MultiserviceRegistration::Reply *matching_reply{};
        for (const auto registration_index :
             std::views::iota(std::size_t{}, catalogue_.multiservice_registrations.size())) {
            for (const auto &reply : multiservice_state_[registration_index].replies) {
                if (reply.service != instance.service ||
                    !same_resolved_signature(reply.signature, instance.signature) ||
                    !instance.type_map.includes(reply.type_map) ||
                    !same_scalars(reply.scalar_arguments, instance.scalar_arguments)) {
                    continue;
                }
                if (matching_reply != nullptr) {
                    throw WiringError{"multiple multiservice replies match service '" +
                                      std::string{instance.signature.name.text()} + "'" + service_paths(instance)};
                }
                matching_reply = &reply;
            }
        }
        if (matching_reply == nullptr) {
            return;
        }

        const auto *expected_output = instance.service->implementation_output_type(instance.signature);
        if (expected_output == nullptr || !expected_output->accepts(matching_reply->output.type())) {
            throw WiringError{"a multiservice reply no longer matches service '" +
                              std::string{instance.signature.name.text()} + "'" + service_paths(instance)};
        }
        wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                              matching_reply->output);
        instance.implementation_replies_wired = true;
        instance.implementation_wired =
            !instance.service->implementation_requests_required() || instance.implementation_requests_wired;
    };

    for (auto instance_index = std::size_t{}; instance_index < requirements_.instances().size(); ++instance_index) {
        auto &instance = *requirements_.instances()[instance_index];
        apply_replies(instance);
        if (!instance.has_client && !instance.implementation_replies_wired) {
            continue;
        }

        auto candidates = std::vector<const State::Registration *>{};
        for (const auto &registration : catalogue_.registrations) {
            if (registration.service == instance.service && instance.type_map.includes(registration.type_map) &&
                matches_scalar_filters(registration.scalar_filters, instance.signature.parameters,
                                       instance.scalar_arguments)) {
                candidates.push_back(&registration);
            }
        }

        auto multiservice_candidates = std::vector<std::size_t>{};
        for (auto index = std::size_t{}; index < catalogue_.multiservice_registrations.size(); ++index) {
            const auto &registration = catalogue_.multiservice_registrations[index];
            auto matching_claims = std::size_t{};
            for (const auto &claim : registration.claims) {
                if (claim.service == instance.service && instance.type_map.includes(claim.type_map) &&
                    matches_scalar_filters(claim.scalar_filters, instance.signature.parameters,
                                           instance.scalar_arguments)) {
                    ++matching_claims;
                }
            }
            if (matching_claims > 1) {
                throw WiringError{"multiservice registration has overlapping claims for service '" +
                                  std::string{instance.signature.name.text()} + "'" + service_paths(instance)};
            }
            if (matching_claims == 1) {
                multiservice_candidates.push_back(index);
            }
        }

        const auto service_name = std::string{instance.signature.name.text()};
        const auto candidate_count = candidates.size() + multiservice_candidates.size();
        if (candidate_count == 0) {
            throw WiringError{"service '" + service_name + "' has no matching implementation" +
                              service_paths(instance)};
        }
        if (candidate_count != 1) {
            throw WiringError{"service '" + service_name + "' has ambiguous implementations" + service_paths(instance)};
        }
        if (instance.implementation_wired) {
            continue;
        }

        if (!multiservice_candidates.empty()) {
            const auto index = multiservice_candidates.front();
            if (multiservice_state_[index].wired) {
                if (instance.service->implementation_requests_required() &&
                    !instance.implementation_requests_wired) {
                    throw WiringError{"multiservice implementation is missing implementation_requests for service '" +
                                      service_name + "'" + service_paths(instance)};
                }
                if (instance.signature.output != nullptr && !instance.implementation_replies_wired) {
                    throw WiringError{"multiservice implementation is missing implementation_replies for service '" +
                                      service_name + "'" + service_paths(instance)};
                }
                throw WiringError{"multiservice implementation did not satisfy service '" + service_name + "'" +
                                  service_paths(instance)};
            }

            wire_multiservice(index, instance);
            continue;
        }

        auto context = WiringContext{wiring_};
        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(instance.scalar_arguments.size() + instance.signature.parameters.size() +
                          candidates.front()->installation_arguments.size());
        if (instance.service->implementation_requests_required()) {
            const auto requests = instance.service->wire_implementation_requests(
                wiring_, instance.signature, details::NestedBindingIdentity::object(&instance), true);
            instance.service->append_implementation_request_arguments(requests, arguments);
        }
        for (const auto &scalar : instance.scalar_arguments) {
            arguments.emplace_back(ScalarValue<>{scalar.view()});
        }
        for (const auto &argument : candidates.front()->installation_arguments) {
            arguments.emplace_back(argument);
        }

        auto output =
            (*candidates.front()->implementation)(instance.type_map, std::span<const WiringArgument>{arguments});
        const auto *expected_output = instance.service->implementation_output_type(instance.signature);

        if (expected_output == nullptr) {
            if (output) {
                throw WiringError{"service '" + service_name + "' implementation produced unexpected output" +
                                  service_paths(instance)};
            }
        } else {
            if (!output || !expected_output->accepts(output->type())) {
                auto message = "service '" + service_name + "' implementation produced an incompatible output";
                if (output) {
                    message += "\n  expected: ";
                    append_type_text(message, *expected_output);
                    message += "\n  received: ";
                    append_type_text(message, output->type());
                } else {
                    message += "\n  expected: ";
                    append_type_text(message, *expected_output);
                    message += "\n  received no output";
                }
                throw WiringError{message + service_paths(instance)};
            }
            wiring_.provide_inherited_time_series(details::NestedBindingIdentity::object(&instance), *expected_output,
                                                  *output);
        }

        instance.implementation_wired = true;
    }
}

void ServiceCatalogue::build(GraphWiring &wiring) const {
    if (!wiring.is_root()) {
        throw std::invalid_argument{"services can only be built on the root GraphWiring"};
    }
    auto *requirements = wiring.service_requirements();
    if (requirements == nullptr) {
        return;
    }

    BuildOperation{*state_, wiring, *requirements}.run();
}

} // namespace bg

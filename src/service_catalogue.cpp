#include <bg/service_catalogue.hpp>

#include <bg/graph/details/inherited_time_series_identity.hpp>
#include <bg/graph/details/service_plan.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>

#include <algorithm>
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
        const Service *service;
        TypeMap type_map;
        std::shared_ptr<const WiringTemplate> implementation;
        std::vector<ScalarFilter> scalar_filters;
    };

    std::vector<Registration> registrations;
};

ServiceCatalogue::ServiceCatalogue() : state_{std::make_unique<State>()} {}

ServiceCatalogue::~ServiceCatalogue() = default;

ServiceCatalogue::ServiceCatalogue(ServiceCatalogue &&) noexcept = default;

ServiceCatalogue &ServiceCatalogue::operator=(ServiceCatalogue &&) noexcept = default;

void ServiceCatalogue::add_impl(const Service &service, TypeMap type_map,
                                std::shared_ptr<const WiringTemplate> implementation,
                                std::span<const WiringArgument> scalar_filters) {
    auto filters = std::vector<State::ScalarFilter>{};
    filters.reserve(scalar_filters.size());
    for (const auto &argument : scalar_filters) {
        if (!argument.name() || !argument.is_scalar() || !argument.present()) {
            throw std::invalid_argument{"a service implementation filter must be a named scalar value"};
        }

        const auto parameter_indices = std::views::iota(std::size_t{}, service.signature().parameter_count());
        const auto parameter = std::ranges::find_if(parameter_indices, [&](std::size_t index) {
            const auto &candidate = service.signature().parameter(index);
            return candidate.name == *argument.name() && candidate.type->is_scalar();
        });
        if (parameter == parameter_indices.end()) {
            throw std::invalid_argument{"a service implementation filter does not name a scalar service parameter"};
        }
        if (std::ranges::contains(filters, *argument.name(), &State::ScalarFilter::name)) {
            throw std::invalid_argument{"a service implementation repeats a scalar filter"};
        }

        filters.push_back({*argument.name(), ScalarValue<>{argument.scalar()}});
    }

    state_->registrations.push_back(
        {&service, std::move(type_map), std::move(implementation), std::move(filters)});
}

void ServiceCatalogue::build(GraphWiring &wiring) const {
    if (wiring.parent_ != nullptr) {
        throw std::invalid_argument{"services can only be built on the root GraphWiring"};
    }
    if (!wiring.service_plan_) {
        return;
    }

    const auto matches_filters = [](const State::Registration &registration,
                                    const details::ServiceInstance &instance) {
        for (const auto &filter : registration.scalar_filters) {
            auto scalar_index = std::size_t{};
            auto matched = false;
            for (const auto &parameter : instance.signature.parameters) {
                if (!parameter.type->is_scalar()) {
                    continue;
                }
                if (parameter.name == filter.name) {
                    matched = instance.scalar_arguments[scalar_index].equals(filter.value);
                    break;
                }
                ++scalar_index;
            }
            if (!matched) {
                return false;
            }
        }
        return true;
    };

    for (auto instance_index = std::size_t{};
         instance_index < wiring.service_plan_->instances().size(); ++instance_index) {
        auto &instance = *wiring.service_plan_->instances()[instance_index];
        if (instance.built) {
            continue;
        }

        auto candidates = std::vector<const State::Registration *>{};
        for (const auto &registration : state_->registrations) {
            if (registration.service == instance.service && instance.type_map.includes(registration.type_map) &&
                matches_filters(registration, instance)) {
                candidates.push_back(&registration);
            }
        }

        const auto service_name = std::string{instance.signature.name.text()};
        if (candidates.empty()) {
            throw WiringError{"service '" + service_name + "' has no matching implementation"};
        }
        if (candidates.size() != 1) {
            throw WiringError{"service '" + service_name + "' has ambiguous implementations"};
        }

        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(instance.scalar_arguments.size());
        for (const auto &scalar : instance.scalar_arguments) {
            arguments.emplace_back(scalar);
        }

        auto context = WiringContext{wiring};
        auto output = (*candidates.front()->implementation)(instance.type_map,
                                                            std::span<const WiringArgument>{arguments});
        if (instance.signature.output == nullptr) {
            if (output) {
                throw WiringError{"service implementation produced an unexpected output"};
            }
        } else {
            if (!output || !instance.signature.output->accepts(output->type())) {
                throw WiringError{"service implementation produced an incompatible output"};
            }
            wiring.provide_inherited_time_series(details::InheritedTimeSeriesIdentity::object(&instance),
                                                 *instance.signature.output, *output);
        }

        instance.built = true;
    }
}

} // namespace bg

#include <bg/graph/service.hpp>

#include <bg/graph/details/inherited_time_series_identity.hpp>
#include <bg/graph/details/service_plan.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace bg {
namespace {

bool contains_reference(const TimeSeriesType &type) {
    if (type_cast<TimeSeriesReferenceType>(&type) != nullptr) {
        return true;
    }
    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        return contains_reference(list->item_type());
    }
    if (const auto *dictionary = type_cast<TimeSeriesDictionaryType>(&type)) {
        return contains_reference(dictionary->value_type());
    }
    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type); bundle != nullptr && bundle->is_resolved()) {
        for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
            if (contains_reference(bundle->item_type(index))) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

Service::Service(ServiceKind kind, Signature signature) : kind_{kind}, signature_{std::move(signature)} {
    auto scalar_parameters_started = false;
    auto time_series_parameters = std::size_t{};
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
        if (contains_reference(static_cast<const TimeSeriesType &>(type))) {
            throw std::invalid_argument{"service signatures cannot contain reference types"};
        }
        ++time_series_parameters;
    }
    if (signature_.output() != nullptr && contains_reference(*signature_.output())) {
        throw std::invalid_argument{"service signatures cannot contain reference types"};
    }

    switch (kind_) {
    case ServiceKind::reference:
        if (time_series_parameters != 0 || !signature_.has_output()) {
            throw std::invalid_argument{"a reference service requires a source-like signature"};
        }
        break;
    case ServiceKind::subscription:
        if (time_series_parameters != 1 ||
            type_cast<TimeSeriesValueType>(signature_.parameter(0).type) == nullptr) {
            throw std::invalid_argument{"a subscription service requires one time series value parameter"};
        }
        break;
    case ServiceKind::compute:
        if (time_series_parameters == 0) {
            throw std::invalid_argument{"a compute service requires at least one time series parameter"};
        }
        break;
    }
}

Service::~Service() = default;

std::optional<WiringPoint> Service::wire(GraphWiring &wiring, ResolvedSignature signature,
                                         const TypeMap &type_map,
                                         std::span<const WiringArgument> arguments) const {
    if (kind_ != ServiceKind::reference) {
        throw WiringError{"subscription and compute service clients are not implemented"};
    }

    for (const auto &parameter : signature.parameters) {
        if (const auto *time_series = type_cast<TimeSeriesType>(parameter.type);
            time_series != nullptr && contains_reference(*time_series)) {
            throw WiringError{"service signatures cannot contain reference types"};
        }
    }
    if (signature.output != nullptr && contains_reference(*signature.output)) {
        throw WiringError{"service signatures cannot contain reference types"};
    }

    return wiring.require_service(*this, std::move(signature), type_map, arguments);
}

details::ServiceInstance &details::ServicePlan::require(const Service &service, ResolvedSignature signature,
                                                        const TypeMap &type_map,
                                                        std::span<const WiringArgument> arguments) {
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
        return **existing;
    }

    auto instance = std::make_unique<ServiceInstance>(
        &service, std::move(signature), type_map.copy(), std::move(scalars));
    auto &result = *instance;
    instances_.push_back(std::move(instance));
    return result;
}

details::ServicePlan &GraphWiring::root_service_plan() {
    if (parent_ != nullptr) {
        return parent_->root_service_plan();
    }
    if (!service_plan_) {
        service_plan_ = std::make_unique<details::ServicePlan>();
    }
    return *service_plan_;
}

std::optional<WiringPoint> GraphWiring::require_service(const Service &service, ResolvedSignature signature,
                                                        const TypeMap &type_map,
                                                        std::span<const WiringArgument> arguments) {
    auto &instance = root_service_plan().require(service, std::move(signature), type_map, arguments);
    if (instance.signature.output == nullptr) {
        return std::nullopt;
    }

    return require_inherited_time_series(details::InheritedTimeSeriesIdentity::object(&instance),
                                         instance.signature.name, *instance.signature.output);
}

} // namespace bg

#include <bg/graph/wiring_template.hpp>

#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <string>
#include <vector>

namespace bg {

const Type &details::wiring_argument::type() const noexcept {
    if (is_wiring_point()) {
        return wiring_point().type();
    }
    return scalar_type();
}

WiringTemplate::~WiringTemplate() = default;

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, SignatureResolution resolution,
                                                           std::span<details::wiring_argument> arguments,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(*this, std::move(resolution), arguments, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, const WiringTemplate &implementation,
                                                           SignatureResolution resolution,
                                                           std::span<details::wiring_argument> arguments,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(implementation, std::move(resolution), arguments, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke(GraphWiring &wiring, std::span<details::wiring_argument> arguments,
                                                  const TypeMap *seed_type_map) const {
    auto argument_types = std::vector<const Type *>{};
    argument_types.reserve(arguments.size());
    for (const auto &argument : arguments) {
        argument_types.push_back(&argument.type());
    }

    auto type_map = signature().match_arguments(argument_types, seed_type_map);
    auto wiring_arguments = WiringArguments{signature(), type_map, arguments};
    auto resolved_signature = signature().complete_resolution(type_map, &wiring_arguments);
    auto resolution = SignatureResolution{std::move(resolved_signature), std::move(type_map)};
    const auto requirement_result = requirement(WiringArguments{signature(), resolution.type_map, arguments});
    if (!requirement_result.accepted()) {
        auto message = std::string{"wiring-template requirement rejected the arguments"};
        if (!requirement_result.message().empty()) {
            message += ": ";
            message += requirement_result.message();
        }
        throw WiringError{std::move(message)};
    }

    const auto path_name = resolution.signature.name;
    return invoke_resolved(wiring, std::move(resolution), arguments, path_name);
}

} // namespace bg

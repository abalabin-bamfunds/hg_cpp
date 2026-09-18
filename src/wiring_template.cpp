#include <bg/graph/wiring_template.hpp>

#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>

#include <string>

namespace bg {

WiringTemplate::~WiringTemplate() = default;

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, SignatureResolution resolution,
                                                           std::span<const WiringArgument> arguments,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(*this, std::move(resolution), arguments, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, const WiringTemplate &implementation,
                                                           SignatureResolution resolution,
                                                           std::span<const WiringArgument> arguments,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(implementation, std::move(resolution), arguments, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke(GraphWiring &wiring,
                                                  std::span<const WiringArgument> arguments,
                                                  const TypeMap *seed_type_map) const {
    auto probe = details::probe_wiring_arguments(signature(), arguments);
    auto resolution =
        signature().resolve_wiring_arguments(probe.arguments, probe.logical_arguments, seed_type_map);
    const auto requirement_result =
        requirement(WiringArguments{signature(), resolution.type_map, probe.logical_arguments});
    if (!requirement_result.accepted()) {
        auto message = std::string{"wiring-template requirement rejected the arguments"};
        if (!requirement_result.message().empty()) {
            message += ": ";
            message += requirement_result.message();
        }
        throw WiringError{std::move(message)};
    }

    const auto path_name = resolution.signature.name;
    auto committed = details::normalize_wiring_arguments(signature(), arguments, wiring.allocator_);
    return invoke_resolved(wiring, std::move(resolution), committed.arguments, path_name);
}

} // namespace bg

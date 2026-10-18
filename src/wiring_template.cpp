#include <bg/graph/wiring_template.hpp>

#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/wiring_record_overlay.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/type/details/resolver_type_resolution_error.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace bg {

WiringTemplate::~WiringTemplate() = default;

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, SignatureResolution resolution,
                                                           std::span<const WiringArgument> arguments,
                                                           std::span<const details::WiringArgumentDescriptor> descriptors,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(*this, std::move(resolution), arguments, descriptors, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke_resolved(GraphWiring &wiring, const WiringTemplate &implementation,
                                                           SignatureResolution resolution,
                                                           std::span<const WiringArgument> arguments,
                                                           std::span<const details::WiringArgumentDescriptor> descriptors,
                                                           util::symbol path_name) const {
    return wiring.invoke_resolved(implementation, std::move(resolution), arguments, descriptors, path_name);
}

std::optional<WiringPoint> WiringTemplate::invoke(GraphWiring &wiring,
                                                  std::span<const WiringArgument> arguments,
                                                  const TypeMap *seed_type_map) const {
    auto temporary_records = details::WiringRecordOverlay{wiring};
    auto probe = details::probe_wiring_arguments(signature(), arguments, temporary_records);
    const auto &declared_signature = signature();
    auto preparation = details::WiringCallPreparation{declared_signature, probe.arguments, seed_type_map};
    if (preparation.empty()) {
        auto supplied_types = std::vector<const Type *>{};
        supplied_types.reserve(probe.arguments.size());
        for (const auto &argument : probe.arguments) {
            supplied_types.push_back(&argument.type());
        }
        (void)declared_signature.match_arguments(supplied_types, seed_type_map);
        throw WiringError{"no advertised wiring form satisfies the wiring-template signature"};
    }
    auto preserved_inputs = std::vector<unsigned char>{};
    preserved_inputs.reserve(probe.arguments.size());
    for (auto index = std::size_t{}; index < probe.arguments.size(); ++index) {
        preserved_inputs.push_back(static_cast<unsigned char>(preserves_input_point(index)));
    }

    struct Candidate {
        details::PreparedWiringCall call;
        double binding_cost;
    };
    auto type_map = TypeMap{};
    auto argument_context = WiringArguments{declared_signature, type_map, probe.descriptors};
    auto requirement_results = std::vector<RequirementResult>{};
    const auto callback_context = WiringErrorContext{
        .stage = WiringFailureStage::callback,
        .path = WiringContext::current().path() == nullptr ? std::string{} : WiringContext::current().path()->text(),
        .operation = declared_signature.name()};
    auto prepared = details::WiringCallPreparationResult{};
    try {
        prepared = preparation.complete(preserved_inputs, argument_context, [&](const TypeMap &resolved_type_map) {
            resolve_additional_types(resolved_type_map);
            try {
                requirement_results.push_back(
                    requirement(WiringArguments{declared_signature, resolved_type_map, probe.descriptors}));
            } catch (const details::ResolverTypeResolutionError &) {
                throw;
            } catch (...) {
                throw WiringError::from_exception("wiring-template requirement callback failed", callback_context,
                                                  std::current_exception());
            }
        });
    } catch (const TypeResolutionError &) {
        throw;
    } catch (const WiringError &error) {
        throw error.with_context(callback_context);
    } catch (...) {
        throw WiringError::from_exception("wiring-template requirement callback failed", callback_context,
                                          std::current_exception());
    }

    auto applicable = std::vector<Candidate>{};
    applicable.reserve(prepared.calls.size());
    auto requirement_message = std::string{};
    for (auto index = std::size_t{}; index < prepared.calls.size(); ++index) {
        const auto &requirement_result = requirement_results[index];
        if (!requirement_result.accepted()) {
            if (requirement_message.empty()) {
                requirement_message = requirement_result.message().empty()
                                         ? "wiring-template requirement rejected the arguments"
                                         : std::string{requirement_result.message()};
            }
            continue;
        }
        const auto binding_cost = prepared.calls[index].binding_cost(probe.arguments);
        applicable.push_back({std::move(prepared.calls[index]), binding_cost});
    }

    if (applicable.empty()) {
        if (prepared.completion_error != nullptr) {
            std::rethrow_exception(prepared.completion_error);
        }
        if (!requirement_message.empty()) {
            throw WiringError{std::move(requirement_message)};
        }
        throw WiringError{"no advertised wiring form resolves the wiring-template signature"};
    }

    const auto output = applicable.front().call.resolution.signature.output;
    if (std::ranges::any_of(applicable, [&](const auto &candidate) {
            return candidate.call.resolution.signature.output != output;
        })) {
        throw WiringError{"viable wiring forms resolve to different wiring-template output types"};
    }
    auto unique_applicable = std::vector<Candidate>{};
    unique_applicable.reserve(applicable.size());
    for (auto &candidate : applicable) {
        const auto existing = std::ranges::find_if(unique_applicable, [&](const auto &other) {
            return candidate.call.same_concrete_binding(other.call) &&
                   same_binding_type_map(candidate.call.resolution.type_map, other.call.resolution.type_map);
        });
        if (existing == unique_applicable.end()) {
            unique_applicable.push_back(std::move(candidate));
        } else if (candidate.binding_cost < existing->binding_cost) {
            *existing = std::move(candidate);
        }
    }

    const auto lowest_cost = std::ranges::min_element(unique_applicable, {}, [](const auto &candidate) {
                                 return candidate.binding_cost;
                             })->binding_cost;
    std::erase_if(unique_applicable, [&](const auto &candidate) {
        return candidate.binding_cost != lowest_cost;
    });

    if (unique_applicable.size() != 1) {
        throw WiringError{"equal-cost wiring forms resolve to distinct concrete bindings"};
    }

    auto selected = std::move(unique_applicable.front().call);
    const auto path_name = selected.resolution.signature.name;
    auto committed = details::normalize_wiring_arguments(declared_signature, arguments, wiring.allocation());
    auto selected_forms = std::vector<const TimeSeriesType *>{};
    selected_forms.reserve(selected.argument_forms.size());
    for (auto index = std::size_t{}; index < selected.argument_forms.size(); ++index) {
        selected_forms.push_back(selected.materialisation_target(index));
    }
    details::bind_materialisation_forms(wiring, *this, committed.arguments, selected_forms);
    return invoke_resolved(wiring, SignatureResolution{std::move(selected.resolution.signature),
                                                       std::move(selected.resolution.type_map)},
                           committed.arguments, committed.descriptors, path_name);
}

} // namespace bg

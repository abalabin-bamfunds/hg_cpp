#include <bg/graph/graph_wiring.hpp>

#include <bg/graph/context.hpp>
#include <bg/graph/details/normalized_wiring_arguments.hpp>
#include <bg/graph/details/wiring_materialisation.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_variable.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/util/scope_exit.hpp>

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace bg {
namespace details {

const Type *PreparedWiringCall::argument_type(std::size_t index, const WiringArgument &argument) const noexcept {
    if (preserves_input_point(index)) {
        return &argument.type();
    }
    return argument_forms[index].offered_type;
}

const TimeSeriesType *PreparedWiringCall::materialisation_target(std::size_t index) const noexcept {
    if (preserves_input_point(index)) {
        return nullptr;
    }
    return argument_forms[index].materialisation_target;
}

bool PreparedWiringCall::same_concrete_binding(const PreparedWiringCall &other) const {
    for (auto index = std::size_t{}; index < argument_forms.size(); ++index) {
        if (preserves_input_point(index)) {
            continue;
        }
        if (resolution.signature.parameters[index].type != other.resolution.signature.parameters[index].type ||
            argument_forms[index].materialisation_target != other.argument_forms[index].materialisation_target) {
            return false;
        }
    }
    return true;
}

double PreparedWiringCall::binding_cost(std::span<const WiringArgument> arguments) const {
    // Reference adaptation costs are charged once per input binding.
    constexpr auto top_level_reference_adaptation_cost = 1.0;
    constexpr auto contained_reference_adaptation_cost = 2.0;

    auto bindings = std::vector<std::pair<const WiringRecord *, const TimeSeriesType *>>{};
    auto total = 0.0;
    for (auto index = std::size_t{}; index < argument_forms.size(); ++index) {
        if (preserves_input_point(index)) {
            continue;
        }

        const auto &form = argument_forms[index];
        if (form.materialisation_target != nullptr) {
            const auto point = arguments[index].time_series();
            const auto binding = std::pair{point.record()->source_identity(), form.materialisation_target};
            if (std::ranges::find(bindings, binding) == bindings.end()) {
                bindings.push_back(binding);
                total += form.materialisation_cost;
            }
        }

        const auto *parameter = type_cast<TimeSeriesType>(resolution.signature.parameters[index].type);
        const auto *offered = type_cast<TimeSeriesType>(form.offered_type);
        if (parameter == nullptr || offered == nullptr) {
            continue;
        }

        const auto *parameter_reference = type_cast<TimeSeriesReferenceType>(parameter);
        const auto *offered_reference = type_cast<TimeSeriesReferenceType>(offered);
        if ((parameter_reference == nullptr) != (offered_reference == nullptr)) {
            total += top_level_reference_adaptation_cost;
        }

        const auto &parameter_target = parameter_reference == nullptr ? *parameter : parameter_reference->target_type();
        const auto &offered_target = offered_reference == nullptr ? *offered : offered_reference->target_type();
        if (parameter_target.requires_reference_representation_for(offered_target)) {
            total += contained_reference_adaptation_cost;
        }
    }
    return total;
}

WiringCallPreparation::WiringCallPreparation(const Signature &signature,
                                             std::span<const WiringArgument> arguments,
                                             const TypeMap *seed_type_map)
    : signature_{&signature}, matches_{signature.match_wiring_arguments(arguments, seed_type_map)} {}

WiringCallPreparationResult WiringCallPreparation::complete(
    std::span<const unsigned char> preserved_inputs, const WiringArguments &argument_context,
    const std::function<void(const TypeMap &)> &completion) {
    auto result = WiringCallPreparationResult{};
    auto matches = std::move(matches_);
    result.calls.reserve(matches.size());
    for (auto &match : matches) {
        try {
            auto resolved = signature_->complete_wiring_match(std::move(match), argument_context, completion);
            result.calls.push_back({std::move(resolved), preserved_inputs});
        } catch (const TypeResolutionError &) {
            if (result.completion_error == nullptr) {
                result.completion_error = std::current_exception();
            }
        }
    }
    return result;
}

void bind_materialisation_forms(GraphWiring &wiring, const WiringTemplate &implementation,
                                std::span<WiringArgument> arguments,
                                std::span<const TimeSeriesType *const> selected_forms) {
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        auto &argument = arguments[index];
        if (!argument.is_time_series() || implementation.preserves_input_point(index)) {
            continue;
        }

        if (const auto *selected_type = selected_forms[index]; selected_type != nullptr) {
            const auto selected = wiring.materialisation_form(argument.time_series(), *selected_type);
            argument.replace_time_series(selected);
            argument.set_resolution_type(*selected_type);
        }
    }
}

void require_concrete_materialisation_forms(GraphWiring &wiring, const WiringTemplate &implementation,
                                            std::span<const WiringArgument> arguments,
                                            std::span<const TimeSeriesType *const> selected_forms) {
    const auto &signature = implementation.signature();
    const auto *path = WiringContext::current().path();
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        if (!arguments[index].is_time_series() || implementation.preserves_input_point(index) ||
            selected_forms[index] == nullptr) {
            continue;
        }
        wiring.require_materialisation(
            arguments[index].time_series(), *selected_forms[index],
            {.path = path == nullptr ? std::string{} : path->text(),
             .consumer = signature.name(),
             .parameter = signature.parameter(index).name});
    }
}

} // namespace details
namespace {

WiringErrorContext request_context(const WiringRequestOrigin &origin, const TimeSeriesType *requested_type) {
    auto requested_text = std::string{};
    if (requested_type != nullptr) {
        requested_type->format_name(requested_text);
    }
    return {.stage = WiringFailureStage::requirement,
            .path = origin.path,
            .operation = origin.consumer,
            .parameter = origin.parameter,
            .requested_type = std::move(requested_text)};
}

WiringErrorContext dependency_context(const details::MaterialisationDependency &dependency) {
    auto requested_text = std::string{};
    if (dependency.target_type != nullptr) {
        dependency.target_type->format_name(requested_text);
    }
    return {.stage = WiringFailureStage::requirement,
            .path = dependency.path,
            .operation = dependency.operation,
            .parameter = dependency.parameter,
            .item = dependency.item,
            .requested_type = std::move(requested_text)};
}

class DeferredWiringRecord final : public details::WiringRecord {
  public:
    DeferredWiringRecord(const TimeSeriesType &semantic_type, std::span<const WiringArgument> arguments)
        : WiringRecord{semantic_type}, arguments_{arguments.begin(), arguments.end()},
          context_{details::ContextSnapshot::capture()}, path_{WiringContext::current().path()},
          multiservice_ports_{WiringContext::current().multiservice_ports_if_available()} {
        const auto diagnostic_contexts = details::WiringDiagnosticContext::current(WiringContext::current().wiring());
        diagnostic_contexts_.assign(diagnostic_contexts.begin(), diagnostic_contexts.end());
    }

    bool has_default_materialisation() const noexcept override { return false; }
    bool materialisation_ready_impl() const noexcept override {
        return std::ranges::all_of(arguments_, [](const WiringArgument &argument) {
            return !argument.is_time_series() || argument.time_series().record()->materialisation_ready();
        });
    }

    void add_materialisation_option(const TimeSeriesType &target_type, double cost, Signature input_signature,
                                    WiringMaterialisationBuilder builder) const override {
        if (options_frozen_) {
            throw WiringError{"materialisation options are fixed after inspection or expansion"};
        }
        if (!target_type.is_resolved()) {
            throw WiringError{"a materialisation option requires a resolved target type"};
        }
        if (!std::isfinite(cost) || cost < 0.0) {
            throw WiringError{"a materialisation option cost must be finite and nonnegative"};
        }
        if (!builder) {
            throw WiringError{"a materialisation option requires a builder"};
        }
        const auto existing = std::ranges::find(options_, &target_type,
                                                [](const auto &option) { return option.target_type; });
        if (existing != options_.end()) {
            throw WiringError{"a wiring point already has an option for this target type"};
        }

        if (input_signature.has_output()) {
            throw WiringError{"a materialisation input signature cannot declare an output"};
        }

        options_.push_back({&target_type, cost, std::move(input_signature), std::move(builder)});
    }

    std::vector<WiringMaterialisationOption> materialisation_options() const override {
        options_frozen_ = true;
        auto descriptors = std::vector<WiringMaterialisationOption>{};
        descriptors.reserve(options_.size());
        for (const auto &option : options_) {
            descriptors.push_back({option.target_type, option.cost, &option.input_signature});
        }
        return descriptors;
    }

    std::optional<WiringErrorContext> materialisation_context(const TimeSeriesType &target_type) const override {
        const auto option = std::ranges::find(options_, &target_type,
                                              [](const auto &candidate) { return candidate.target_type; });
        if (option == options_.end()) {
            return std::nullopt;
        }
        auto requested_text = std::string{};
        target_type.format_name(requested_text);
        return WiringErrorContext{.stage = WiringFailureStage::callback,
                                  .path = path_ == nullptr ? std::string{} : path_->text(),
                                  .operation = option->input_signature.name(),
                                  .requested_type = std::move(requested_text)};
    }

    details::MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override {
        options_frozen_ = true;
        auto context = context_.activate();
        auto wiring_context = std::optional<WiringContext>{};
        if (path_ != nullptr && multiservice_ports_ != nullptr) {
            wiring_context.emplace(wiring(), *path_, *multiservice_ports_);
        } else if (path_ != nullptr) {
            wiring_context.emplace(wiring(), *path_);
        } else if (multiservice_ports_ != nullptr) {
            wiring_context.emplace(wiring(), *multiservice_ports_);
        } else {
            wiring_context.emplace(wiring());
        }

        const auto option = std::ranges::find(options_, &target_type,
                                              [](const auto &candidate) { return candidate.target_type; });
        if (option == options_.end()) {
            throw WiringError{"a wiring point has no materialisation option for the requested target type"};
        }

        auto preparation = details::MaterialisationPreparation{};
        auto normalized = details::normalize_wiring_arguments(option->input_signature, arguments_,
                                                              wiring().allocation(), &option->input_signature);
        auto call_preparation = details::WiringCallPreparation{option->input_signature, normalized.arguments};
        if (call_preparation.empty()) {
            throw WiringError{"a materialisation option's input signature does not match its retained arguments"};
        }
        auto preserved_inputs = std::vector<unsigned char>(normalized.arguments.size());
        for (auto index = std::size_t{}; index < preserved_inputs.size(); ++index) {
            preserved_inputs[index] = static_cast<unsigned char>(option->input_signature.is_union_parameter(index));
        }

        auto type_map = TypeMap{};
        auto argument_context = WiringArguments{option->input_signature, type_map, normalized.descriptors};
        auto prepared = call_preparation.complete(preserved_inputs, argument_context);
        if (prepared.calls.empty()) {
            auto completion_error = std::string{};
            try {
                std::rethrow_exception(prepared.completion_error);
            } catch (const TypeResolutionError &error) {
                completion_error = error.what();
            }
            throw WiringError{"materialisation inputs cannot resolve: " + completion_error};
        }

        const auto binding_cost = [&](const auto &match) {
            return match.binding_cost(normalized.arguments);
        };

        auto best_cost = binding_cost(prepared.calls.front());
        for (std::size_t index = 1; index < prepared.calls.size(); ++index) {
            best_cost = std::min(best_cost, binding_cost(prepared.calls[index]));
        }
        auto best = std::optional<std::size_t>{};
        for (std::size_t index = 0; index < prepared.calls.size(); ++index) {
            if (binding_cost(prepared.calls[index]) != best_cost) {
                continue;
            }
            if (best && !prepared.calls[*best].same_concrete_binding(prepared.calls[index])) {
                throw WiringError{"a materialisation option's input signature has ambiguous concrete forms"};
            }
            best = index;
        }

        preparation.input_targets.resize(normalized.arguments.size());
        for (std::size_t index = 0; index < normalized.arguments.size(); ++index) {
            if (prepared.calls[*best].preserves_input_point(index)) {
                continue;
            }
            const auto &argument = normalized.arguments[index];
            const auto *input_target = prepared.calls[*best].materialisation_target(index);
            if (input_target != nullptr && !argument.is_time_series()) {
                throw WiringError{"a materialisation form requires a time series argument"};
            }
            preparation.input_targets[index] = input_target;
            if (!argument.is_time_series()) {
                continue;
            }
            const auto *input = type_cast<TimeSeriesType>(
                prepared.calls[*best].resolution.signature.parameters[index].type);
            if (input == nullptr) {
                continue;
            }
            preparation.dependencies.push_back(
                {argument.time_series(), input_target == nullptr ? input : input_target,
                 option->input_signature.name(), path_ == nullptr ? std::string{} : path_->text(),
                 option->input_signature.parameter(index).name, std::nullopt});
        }
        preparation.arguments = std::move(normalized.arguments);
        return preparation;
    }

    WiringPoint expand_materialisation(const TimeSeriesType &target_type,
                                       std::span<const WiringArgument> arguments,
                                       std::span<const TimeSeriesType *const> input_targets) const override {
        auto diagnostics = details::WiringDiagnosticContext{wiring(), diagnostic_contexts_};
        options_frozen_ = true;
        const auto option = std::ranges::find(options_, &target_type,
                                              [](const auto &candidate) { return candidate.target_type; });
        if (option == options_.end()) {
            throw WiringError{"a wiring point has no materialisation option for the requested target type"};
        }

        if (input_targets.size() != arguments.size()) {
            throw WiringError{"prepared materialisation inputs do not match the callback signature"};
        }
        auto callback_arguments = std::vector<WiringArgument>{};
        callback_arguments.reserve(arguments.size());
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            auto argument = WiringArgument::borrow(arguments[index]);
            if (input_targets[index] != nullptr) {
                const auto expanded = wiring().expand_materialisation(
                    argument.time_series(), *input_targets[index]);
                argument.replace_time_series(expanded);
                argument.set_resolution_type(expanded.type());
            }
            callback_arguments.push_back(std::move(argument));
        }

        auto context = context_.activate();
        const auto invoke_builder = [&] {
            try {
                return option->builder(callback_arguments);
            } catch (...) {
                const auto callback_context = *materialisation_context(target_type);
                throw WiringError::from_exception("materialisation builder callback failed",
                                                  std::move(callback_context), std::current_exception());
            }
        };
        if (path_ != nullptr && multiservice_ports_ != nullptr) {
            auto wiring_context = WiringContext{wiring(), *path_, *multiservice_ports_};
            return invoke_builder();
        }
        if (path_ != nullptr) {
            auto wiring_context = WiringContext{wiring(), *path_};
            return invoke_builder();
        }
        if (multiservice_ports_ != nullptr) {
            auto wiring_context = WiringContext{wiring(), *multiservice_ports_};
            return invoke_builder();
        }
        auto wiring_context = WiringContext{wiring()};
        return invoke_builder();
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        return wiring().expand_materialisation(point(*this), requested_type);
    }

  private:
    struct Option {
        const TimeSeriesType *target_type;
        double cost;
        Signature input_signature;
        WiringMaterialisationBuilder builder;
    };

    std::vector<WiringArgument> arguments_;
    details::ContextSnapshot context_;
    const WiringPath *path_;
    details::MultiservicePorts *multiservice_ports_;
    std::vector<WiringErrorContext> diagnostic_contexts_;
    mutable std::vector<Option> options_;
    mutable bool options_frozen_{};
};

} // namespace

WiringPoint GraphWiring::create_wp(const TimeSeriesType &semantic_type, std::span<const WiringArgument> arguments) {
    if (!semantic_type.is_resolved()) {
        throw WiringError{"a deferred wiring point requires a resolved semantic type"};
    }
    for (const auto &argument : arguments) {
        if (argument.is_time_series()) {
            const auto point = argument.time_series();
            if (&point.graph() != this) {
                throw WiringError{"a deferred wiring point argument belongs to another graph wiring"};
            }
            if (!point.record()->graph_owned()) {
                throw WiringError{"a deferred wiring point cannot retain a temporary overlay record"};
            }
        }
    }

    return register_record(std::make_unique<DeferredWiringRecord>(semantic_type, arguments));
}

void GraphWiring::add_materialisation_option(WiringPoint point, const TimeSeriesType &target_type, double cost,
                                             Signature input_signature, WiringMaterialisationBuilder builder) {
    ensure_open();
    if (!point.has_output() || &point.graph() != this || !point.record()->graph_owned()) {
        throw WiringError{"a materialisation option requires a wiring point from this graph wiring"};
    }

    point.record()->add_materialisation_option(target_type, cost, std::move(input_signature), std::move(builder));
}

std::vector<WiringMaterialisationOption> GraphWiring::materialisation_options(WiringPoint point) {
    if (!point.has_output() || &point.graph() != this) {
        throw WiringError{"materialisation options require a wiring point from this graph wiring"};
    }

    return point.record()->materialisation_options();
}

void GraphWiring::require_materialisation(WiringPoint point, const TimeSeriesType &target_type) {
    require_materialisation(point, target_type, {});
}

void GraphWiring::require_materialisation(WiringPoint point, const TimeSeriesType &target_type,
                                          WiringRequestOrigin origin) {
    if (!point.has_output() || &point.graph() != this || !point.record()->graph_owned()) {
        throw WiringError{"materialisation requirements need a wiring point from this graph wiring"};
    }
    if (!target_type.is_resolved()) {
        throw WiringError{"a materialisation requirement needs a resolved target type"};
    }
    ensure_open();

    struct OriginUpdate {
        std::size_t requirement;
        std::vector<WiringErrorContext> origins;
    };

    auto root_origin = std::vector<WiringErrorContext>{};
    const auto active_contexts = details::WiringDiagnosticContext::current(*this);
    root_origin.insert(root_origin.end(), active_contexts.begin(), active_contexts.end());
    root_origin.push_back(request_context(origin, &target_type));
    auto staged_requirements = std::vector<MaterialisationRequirement>{};
    auto staged_preparations = std::vector<PreparedMaterialisation>{};
    auto origin_updates = std::vector<OriginUpdate>{};

    const auto find_requirement = [&](const details::WiringRecord *record, const TimeSeriesType *type) {
        return std::ranges::find_if(materialisation_requirements_, [&](const auto &requirement) {
            return requirement.record->source_identity() == record->source_identity() && requirement.type == type;
        });
    };
    const auto find_staged_requirement = [&](const details::WiringRecord *record, const TimeSeriesType *type) {
        return std::ranges::find_if(staged_requirements, [&](const auto &requirement) {
            return requirement.record->source_identity() == record->source_identity() && requirement.type == type;
        });
    };
    const auto find_preparation = [&](const details::WiringRecord *record, const TimeSeriesType *type) {
        return std::ranges::find_if(prepared_materialisations_, [&](const auto &entry) {
            return entry.record == record && entry.type == type;
        });
    };
    const auto find_staged_preparation = [&](const details::WiringRecord *record, const TimeSeriesType *type) {
        return std::ranges::find_if(staged_preparations, [&](const auto &entry) {
            return entry.record == record && entry.type == type;
        });
    };
    const auto add_origins = [&](const details::WiringRecord *record, const TimeSeriesType *type,
                                 std::span<const WiringErrorContext> request_origins) {
        auto newly_known = std::vector<WiringErrorContext>{};
        const auto existing = find_requirement(record, type);
        if (existing != materialisation_requirements_.end()) {
            const auto index = static_cast<std::size_t>(existing - materialisation_requirements_.begin());
            auto update = std::ranges::find(origin_updates, index, &OriginUpdate::requirement);
            if (update == origin_updates.end()) {
                origin_updates.push_back({index, {}});
                update = std::prev(origin_updates.end());
            }
            for (const auto &origin : request_origins) {
                const auto already_present =
                    std::ranges::find(existing->origins, origin) != existing->origins.end() ||
                    std::ranges::find(update->origins, origin) != update->origins.end();
                if (!already_present) {
                    update->origins.push_back(origin);
                    newly_known.push_back(origin);
                }
            }
            return newly_known;
        }

        auto staged = find_staged_requirement(record, type);
        if (staged == staged_requirements.end()) {
            staged_requirements.push_back({record, type, {}});
            staged = std::prev(staged_requirements.end());
        }
        for (const auto &origin : request_origins) {
            if (std::ranges::find(staged->origins, origin) == staged->origins.end()) {
                staged->origins.push_back(origin);
                newly_known.push_back(origin);
            }
        }
        return newly_known;
    };

    const auto collect = [&](auto &&self, WiringPoint source, const TimeSeriesType &target,
                             std::vector<WiringErrorContext> request_origins) -> void {
        if (!source.has_output() || &source.graph() != this || !source.record()->graph_owned()) {
            throw WiringError::from_exception("a materialisation dependency must belong to this graph wiring",
                                              request_origins, {});
        }

        auto *record = source.record();
        if (const auto context = record->materialisation_context(target);
            context && std::ranges::find(request_origins, *context) == request_origins.end()) {
            request_origins.push_back(*context);
        }
        const auto existing_requirement = find_requirement(record, &target);
        if (existing_requirement != materialisation_requirements_.end()) {
            record = existing_requirement->record;
        } else {
            const auto staged_requirement = find_staged_requirement(record, &target);
            if (staged_requirement != staged_requirements.end()) {
                record = staged_requirement->record;
            }
        }
        auto new_origins = add_origins(record, &target, request_origins);
        if (new_origins.empty()) {
            return;
        }

        const auto cached_preparation = find_preparation(record, &target);
        const auto staged_preparation = find_staged_preparation(record, &target);
        const auto needs_preparation = cached_preparation == prepared_materialisations_.end() &&
                                       staged_preparation == staged_preparations.end();
        auto preparation = std::optional<details::MaterialisationPreparation>{};
        auto dependencies = std::vector<details::MaterialisationDependency>{};
        auto argument_count = std::size_t{};
        auto target_count = std::size_t{};
        try {
            if (cached_preparation != prepared_materialisations_.end()) {
                argument_count = cached_preparation->arguments.size();
                target_count = cached_preparation->input_targets.size();
                dependencies = cached_preparation->dependencies;
            } else if (staged_preparation != staged_preparations.end()) {
                argument_count = staged_preparation->arguments.size();
                target_count = staged_preparation->input_targets.size();
                dependencies = staged_preparation->dependencies;
            } else {
                preparation.emplace(record->prepare_materialisation(target));
                argument_count = preparation->arguments.size();
                target_count = preparation->input_targets.size();
                dependencies = preparation->dependencies;
            }
        } catch (...) {
            throw WiringError::from_exception("failed to prepare materialisation", request_origins,
                                              std::current_exception());
        }
        if (argument_count != target_count) {
            throw WiringError::from_exception(
                "prepared materialisation inputs do not match their target bindings", request_origins, {});
        }
        if (needs_preparation) {
            staged_preparations.push_back(
                {record, &target, std::move(preparation->arguments), std::move(preparation->input_targets),
                 std::move(preparation->dependencies)});
        }
        for (const auto &dependency : dependencies) {
            if (dependency.target_type == nullptr) {
                throw WiringError::from_exception("a materialisation dependency has no target type", request_origins,
                                                  {});
            }
            auto dependency_origin = new_origins;
            const auto context = dependency_context(dependency);
            if (std::ranges::find(dependency_origin, context) == dependency_origin.end()) {
                dependency_origin.push_back(context);
            }
            self(self, dependency.point, *dependency.target_type, std::move(dependency_origin));
        }
    };
    collect(collect, point, target_type, std::move(root_origin));

    materialisation_requirements_.reserve(materialisation_requirements_.size() + staged_requirements.size());
    prepared_materialisations_.reserve(prepared_materialisations_.size() + staged_preparations.size());
    for (auto &update : origin_updates) {
        auto &existing = materialisation_requirements_[update.requirement].origins;
        existing.reserve(existing.size() + update.origins.size());
    }
    for (auto &update : origin_updates) {
        auto &existing = materialisation_requirements_[update.requirement].origins;
        existing.insert(existing.end(), std::make_move_iterator(update.origins.begin()),
                        std::make_move_iterator(update.origins.end()));
    }
    materialisation_requirements_.insert(materialisation_requirements_.end(), staged_requirements.begin(),
                                         staged_requirements.end());
    for (auto &preparation : staged_preparations) {
        prepared_materialisations_.push_back(std::move(preparation));
    }
}

bool GraphWiring::materialisation_required(WiringPoint point, const TimeSeriesType &target_type) const {
    if (!point.has_output() || &point.graph() != this || !point.record()->graph_owned()) {
        throw WiringError{"a materialisation query needs a wiring point from this graph wiring"};
    }
    if (!target_type.is_resolved()) {
        throw WiringError{"a materialisation query needs a resolved target type"};
    }

    return std::ranges::any_of(materialisation_requirements_, [&](const auto &entry) {
        return entry.record->source_identity() == point.record()->source_identity() && entry.type == &target_type;
    });
}

WiringPoint GraphWiring::expand_materialisation(WiringPoint point, const TimeSeriesType &target_type) {
    if (!point.has_output() || &point.graph() != this || !point.record()->graph_owned()) {
        throw WiringError{"materialisation expansion requires a wiring point from this graph wiring"};
    }
    if (!target_type.is_resolved()) {
        throw WiringError{"materialisation expansion requires a resolved target type"};
    }

    const auto *record = point.record();
    const auto existing = std::ranges::find_if(expanded_wiring_points_, [&](const auto &entry) {
        return entry.record == record && entry.type == &target_type;
    });
    if (existing != expanded_wiring_points_.end()) {
        return details::WiringRecord::point(*existing->result);
    }
    ensure_open();
    if (std::ranges::any_of(active_materialisation_expansions_, [&](const auto &request) {
            return request.record == record && request.type == &target_type;
        })) {
        throw WiringError{"materialisation expansion contains a dependency cycle"};
    }
    active_materialisation_expansions_.push_back({record, &target_type});
    auto remove_active_request =
        util::scope_exit{[&]() noexcept { active_materialisation_expansions_.pop_back(); }};

    auto requested_text = std::string{};
    target_type.format_name(requested_text);
    auto expansion_context = record->materialisation_context(target_type).value_or(
        WiringErrorContext{.stage = WiringFailureStage::callback, .requested_type = std::move(requested_text)});
    auto diagnostic_context = details::WiringDiagnosticContext{*this, std::span{&expansion_context, std::size_t{1}}};
    auto preparation = std::ranges::find_if(prepared_materialisations_, [&](const auto &entry) {
        return entry.record == record && entry.type == &target_type;
    });
    if (preparation == prepared_materialisations_.end()) {
        try {
            auto prepared = record->prepare_materialisation(target_type);
            if (prepared.arguments.size() != prepared.input_targets.size()) {
                throw WiringError{"prepared materialisation inputs do not match their target bindings"};
            }
            prepared_materialisations_.reserve(prepared_materialisations_.size() + 1);
            prepared_materialisations_.push_back(
                {record, &target_type, std::move(prepared.arguments), std::move(prepared.input_targets),
                 std::move(prepared.dependencies)});
            preparation = std::ranges::find_if(prepared_materialisations_, [&](const auto &entry) {
                return entry.record == record && entry.type == &target_type;
            });
        } catch (...) {
            throw WiringError::from_exception("materialisation preparation failed", expansion_context,
                                              std::current_exception());
        }
    }

    auto result = WiringPoint{};
    try {
        result = record->expand_materialisation(target_type, preparation->arguments, preparation->input_targets);
    } catch (...) {
        throw WiringError::from_exception("materialisation expansion failed", expansion_context,
                                          std::current_exception());
    }
    if (!result.has_output()) {
        throw WiringError{"materialisation expansion returned no point"}.with_context(expansion_context);
    }
    if (&result.graph() != this) {
        throw WiringError{"materialisation expansion returned a point from another graph wiring"}
            .with_context(expansion_context);
    }
    if (!result.record()->graph_owned()) {
        throw WiringError{"materialisation expansion returned a temporary overlay record"}
            .with_context(expansion_context);
    }
    if (!target_type.accepts(result.type())) {
        auto context = expansion_context;
        target_type.format_name(context.expected_type);
        result.type().format_name(context.actual_type);
        throw WiringError{"materialisation expansion returned an incompatible type"}.with_context(std::move(context));
    }

    expanded_wiring_points_.push_back({record, &target_type, result.record()});
    return result;
}

} // namespace bg

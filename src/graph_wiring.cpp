#include <bg/graph/graph_wiring.hpp>
#include <bg/service_catalogue.hpp>

#include <bg/graph/details/service_requirements.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/context.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/util/scope_exit.hpp>

#include "time_series/ref/ref_node.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace bg {

struct GraphWiring::PendingNodeCall {
    enum class State : std::uint8_t { pending, resolving, resolved };

    PendingNodeCall(const NodeTemplate &node_template, ResolvedSignature signature, const TypeMap &type_map,
                    std::span<const WiringArgument> arguments, const TimeSeriesRuntimeType *output_runtime_type,
                    const WiringPath *path,
                    details::MultiservicePorts *multiservice_ports)
        : node_template{&node_template}, signature{std::move(signature)}, type_map{type_map.copy()},
          arguments{arguments.begin(), arguments.end()}, output_runtime_type{output_runtime_type},
          context{details::ContextSnapshot::capture()}, path{path}, multiservice_ports{multiservice_ports} {
        const auto diagnostic_contexts = details::WiringDiagnosticContext::current(WiringContext::current().wiring());
        this->diagnostic_contexts.assign(diagnostic_contexts.begin(), diagnostic_contexts.end());
    }

    const NodeTemplate *node_template;
    ResolvedSignature signature;
    TypeMap type_map;
    std::vector<WiringArgument> arguments;
    const TimeSeriesRuntimeType *output_runtime_type;
    details::ContextSnapshot context;
    const WiringPath *path;
    details::MultiservicePorts *multiservice_ports;
    std::vector<WiringErrorContext> diagnostic_contexts;
    State state{State::pending};
    std::optional<WiringPoint> pending_output;
    details::WiringRecord *pending_output_record{};
    std::optional<WiringPoint> output;
    std::optional<std::size_t> concrete_node_index;
};

const TimeSeriesType &GraphWiring::InheritedTimeSeriesRequirement::boundary_input_type() const {
    if (type_cast<TimeSeriesReferenceType>(type) != nullptr) {
        return *type;
    }
    return time_series_reference_type(*type, TimeSeriesGuarantee::const_);
}

namespace details {

class InheritedTimeSeriesWiringRecord final : public WiringRecord {
  public:
    explicit InheritedTimeSeriesWiringRecord(const TimeSeriesType &type) : WiringRecord{type} {}

  private:
    WiringPoint materialize(const TimeSeriesType &) const override {
        return materialize_boundary_reference(point(*this));
    }
};

class PendingNodeOutputWiringRecord final : public WiringRecord {
  public:
    PendingNodeOutputWiringRecord(const TimeSeriesType &type, std::size_t identity)
        : WiringRecord{type}, identity_{identity} {}

    std::optional<NodeCallIdentity> node_call_identity() const noexcept override {
        return NodeCallIdentity{identity_};
    }

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto output = wiring().resolve_pending_node_call(identity_);
        return wiring().materialize(output, requested_type);
    }

    std::size_t identity_;
};

} // namespace details

namespace {

util::linear_hash_table::hash_type output_item_hash(std::size_t parent, item_id_t item) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    auto result = parent;
    result ^= static_cast<std::size_t>(item) + constant + (result << 6) + (result >> 2);
    return util::linear_hash_table::hash_code(result);
}

std::optional<Scalar<>> scalar_value(const WiringArgument &argument) {
    if (!argument.present()) {
        return std::nullopt;
    }
    return argument.scalar();
}

bool same_scalar_arguments(std::span<const WiredScalar> stored, std::span<const WiringArgument> arguments) {
    auto scalar_index = std::size_t{};
    for (const auto &argument : arguments) {
        if (!argument.is_scalar()) {
            continue;
        }

        const auto &candidate = stored[scalar_index++];
        if (candidate.has_value() != argument.present()) {
            return false;
        }
        if (!candidate.has_value()) {
            continue;
        }

        const auto current = scalar_value(argument);
        const auto stored_value = Scalar<>::from(candidate.runtime_type(), candidate.data());
        if (!stored_value.equals(*current)) {
            return false;
        }
    }

    return scalar_index == stored.size();
}

} // namespace

const GraphWiring &WiringPoint::graph() const {
    if (!has_output()) {
        throw WiringError{"a wiring point has no output"};
    }
    return record_->wiring();
}

const TimeSeriesType &WiringPoint::type() const {
    if (!has_output()) {
        throw WiringError{"a wiring point has no output"};
    }
    return record_->type();
}

GraphWiring::GraphWiring() : GraphWiring(allocator{}) {}

GraphWiring::GraphWiring(allocator memory)
    : allocator_{memory}, fixed_memory_{&allocator_.memory_resource()},
      variable_allocator_{*std::pmr::null_memory_resource()}, paths_{&fixed_memory_} {}

GraphWiring::GraphWiring(GraphWiring &parent) : GraphWiring(parent, allocator{}) {}

GraphWiring::GraphWiring(GraphWiring &parent, allocator memory)
    : allocator_{memory}, parent_{&parent}, fixed_memory_{&allocator_.memory_resource()},
      variable_allocator_{*std::pmr::null_memory_resource()}, paths_{&fixed_memory_} {}

void GraphWiring::ensure_open() const {
    if (finalization_state_ == FinalizationState::finalized || finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a closed GraphWiring cannot accept more construction"};
    }
}

allocator &GraphWiring::variable_allocator() noexcept {
    return variable_allocator_;
}

allocator &GraphWiring::variable_allocator(const ScalarRuntimeType &runtime_type) {
    if (runtime_type.uses_variable_memory() && !variable_memory_) {
        variable_memory_.emplace(&allocator_.memory_resource());
        variable_allocator_ = allocator{*variable_memory_};
    }

    return variable_allocator();
}

GraphWiring::~GraphWiring() {
    for (const auto &node : nodes_) {
        for (const auto &scalar : node.scalar_values()) {
            if (scalar.has_value() && scalar.runtime_type_->requires_destruction()) {
                auto memory = variable_allocator();
                scalar.runtime_type_->destroy(scalar.data_, memory);
            }
        }
    }

    wired_nodes_.release(fixed_memory_);
    output_items_.release(fixed_memory_);
}

const NodeTemplate &GraphWiring::own_node_template(std::unique_ptr<NodeTemplate> node_template) {
    ensure_open();
    auto &result = *node_template;
    node_templates_.push_back(std::move(node_template));
    return result;
}

void GraphWiring::destroy_scalars(std::span<const WiredScalar> scalars) noexcept {
    for (const auto &scalar : scalars) {
        if (!scalar.has_value()) {
            continue;
        }

        auto memory = variable_allocator();
        scalar.runtime_type_->destroy(scalar.data_, memory);
        fixed_memory_.deallocate(scalar.data_, scalar.runtime_type_->size(), scalar.runtime_type_->alignment());
    }
}

util::symbol GraphWiring::default_label() const {
    std::array<char, std::numeric_limits<std::size_t>::digits10 + 2> buffer;
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), paths_.size());
    if (error != std::errc{}) {
        throw WiringError{"failed to create a default wiring label"};
    }

    return util::symbol{std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())}};
}

const WiringPath &GraphWiring::make_path(util::symbol name, util::symbol label) {
    paths_.push_back({WiringContext::current().path(), name, label});

    return paths_.back();
}

std::optional<WiringPoint> GraphWiring::invoke(const WiringTemplate &wiring_template,
                                               std::span<const WiringArgument> arguments,
                                               const TypeMap *seed_type_map) {
    for (const auto &argument : arguments) {
        if (argument.is_time_series()) {
            const auto source = argument.time_series();
            if (&source.graph() != this) {
                throw WiringError{"a WiringPoint belongs to another GraphWiring"};
            }
        }
    }

    return wiring_template.invoke(*this, arguments, seed_type_map);
}

GraphWiring::NodeCallResult GraphWiring::invoke_node(const NodeTemplate &node_template,
                                                     std::span<const WiringArgument> arguments) {
    auto capture = NodeCallCapture{&node_template, std::nullopt};
    node_call_captures_.push_back(&capture);
    auto remove_capture = util::scope_exit{[&]() noexcept { node_call_captures_.pop_back(); }};

    auto output = invoke(node_template, arguments);
    if (!capture.identity) {
        throw WiringError{"a node invocation did not collect a node call"};
    }
    return {NodeCallIdentity{*capture.identity}, std::move(output)};
}

std::optional<WiringPoint> GraphWiring::invoke_resolved(const WiringTemplate &wiring_template,
                                                        SignatureResolution resolution,
                                                        std::span<const WiringArgument> arguments,
                                                        util::symbol path_name) {
    if (finalization_state_ == FinalizationState::finalized || finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a finalized GraphWiring cannot accept more calls"};
    }

    auto signature = std::move(resolution.signature);

    const auto *expected_output = signature.output;
    const auto label = wiring_template.label(signature, arguments, default_label());
    const auto &path = make_path(path_name, label);
    auto context = WiringContext{*this, path};
    auto call_context = WiringErrorContext{.stage = WiringFailureStage::callback,
                                           .path = path.text(),
                                           .operation = signature.name};
    auto diagnostics = details::WiringDiagnosticContext{*this, std::span{&call_context, std::size_t{1}}};
    auto output = std::optional<WiringPoint>{};
    try {
        output = wiring_template.wire(*this, std::move(signature), resolution.type_map, arguments);
    } catch (...) {
        throw WiringError::from_exception("wiring callback failed", call_context, std::current_exception());
    }

    try {
        if ((expected_output == nullptr) != !output.has_value()) {
            auto mismatch = call_context;
            mismatch.expected_type = expected_output == nullptr ? "no output" : std::string{};
            mismatch.actual_type = output ? std::string{} : "no output";
            if (expected_output != nullptr) {
                expected_output->format_name(mismatch.expected_type);
            }
            if (output) {
                output->type().format_name(mismatch.actual_type);
            }
            throw WiringError{"a wiring-template result does not match its output declaration"}
                .with_context(std::move(mismatch));
        }

        if (!output) {
            return std::nullopt;
        }

        if (&output->graph() != this) {
            throw WiringError{"a wiring-template returned a WiringPoint from another GraphWiring"};
        }

        if (!expected_output->accepts(output->type())) {
            auto mismatch = call_context;
            expected_output->format_name(mismatch.expected_type);
            output->type().format_name(mismatch.actual_type);
            throw WiringError{"a wiring-template result does not satisfy its resolved output type"}
                .with_context(std::move(mismatch));
        }

        return output;
    } catch (...) {
        throw WiringError::from_exception("invalid wiring-template result", call_context,
                                          std::current_exception());
    }
}

WiringPoint GraphWiring::defer_node_call(const NodeTemplate &node_template, ResolvedSignature signature,
                                        const TypeMap &type_map,
                                        std::span<const WiringArgument> arguments,
                                        const TimeSeriesRuntimeType *output_runtime_type) {
    if (finalization_state_ == FinalizationState::finalized || finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a finalized GraphWiring cannot accept more node calls"};
    }
    auto pending = std::make_unique<PendingNodeCall>(node_template, std::move(signature), type_map, arguments,
                                                     output_runtime_type, WiringContext::current().path(),
                                                     WiringContext::current().multiservice_ports_if_available());
    for (auto index = std::size_t{}; index < pending->arguments.size(); ++index) {
        const auto &argument = pending->arguments[index];
        if (!argument.is_time_series()) {
            continue;
        }

        const auto *target = type_cast<TimeSeriesType>(pending->signature.parameters[index].type);
        if (target == nullptr) {
            throw WiringError{"a time series node input has no resolved time series parameter type"};
        }
        require_materialisation(argument.time_series(), *target,
                               {.path = pending->path == nullptr ? std::string{} : pending->path->text(),
                                .consumer = pending->signature.name,
                                .parameter = pending->signature.parameters[index].name});
    }

    const auto identity = pending_node_calls_.size();
    auto result = WiringPoint{};
    if (pending->signature.output != nullptr) {
        auto output_record = std::make_unique<details::PendingNodeOutputWiringRecord>(
            *pending->signature.output, identity);
        pending->pending_output_record = output_record.get();
        result = register_record(std::move(output_record));
        pending->pending_output = result;
    }
    pending_node_calls_.push_back(std::move(pending));
    if (!node_call_captures_.empty() && node_call_captures_.back()->node_template == &node_template) {
        auto &captured_identity = node_call_captures_.back()->identity;
        if (captured_identity) {
            throw WiringError{"a node invocation collected more than one node call"};
        }
        captured_identity = identity;
    }
    return result;
}

WiringPoint GraphWiring::resolve_pending_node_call(std::size_t identity) {
    if (identity >= pending_node_calls_.size()) {
        throw WiringError{"a pending node identity is not in this GraphWiring"};
    }

    auto &call = *pending_node_calls_[identity];
    if (call.state == PendingNodeCall::State::resolved) {
        return call.output.value_or(WiringPoint{});
    }
    if (call.state == PendingNodeCall::State::resolving) {
        throw WiringError{"pending node calls contain a dependency cycle"};
    }
    if (finalization_state_ != FinalizationState::finalizing) {
        throw WiringError{"a pending node call can only be bound during GraphWiring construction completion"};
    }

    call.state = PendingNodeCall::State::resolving;
    active_pending_node_calls_.push_back(identity);
    auto restore_dispatch = util::scope_exit{[&]() noexcept {
        active_pending_node_calls_.pop_back();
    }};

    auto context = call.context.activate();
    auto diagnostics = details::WiringDiagnosticContext{*this, call.diagnostic_contexts};
    auto wiring_context = call.multiservice_ports == nullptr
                              ? WiringContext{*this, *call.path}
                              : WiringContext{*this, *call.path, *call.multiservice_ports};
    const auto expected_output = call.signature.output;
    auto output = call.node_template->bind(*this, call.signature, call.type_map, call.arguments,
                                          call.output_runtime_type);
    if ((expected_output == nullptr) != !output.has_value()) {
        throw WiringError{"a pending node result does not match its output declaration"};
    }

    if (output) {
        if (&output->graph() != this || !expected_output->accepts(output->type())) {
            throw WiringError{"a pending node result has an incompatible wiring point"};
        }
        if (!output->record()->has_wired_output()) {
            throw WiringError{"a pending node result has no concrete wired output"};
        }
        call.concrete_node_index = output_node_index(*output);
        if (call.pending_output) {
            call.pending_output_record->set_wired_output_index(output_index(*output));
        }
        call.output = *output;
    } else if (!call.concrete_node_index) {
        throw WiringError{"an outputless pending node did not register a concrete node"};
    }

    call.state = PendingNodeCall::State::resolved;
    return call.output.value_or(WiringPoint{});
}

void GraphWiring::finalize() {
    if (finalization_state_ == FinalizationState::finalized) {
        return;
    }
    if (finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a failed GraphWiring cannot be finalized"};
    }
    if (finalization_state_ == FinalizationState::finalizing) {
        throw WiringError{"GraphWiring finalization is already active"};
    }

    finalization_state_ = FinalizationState::finalizing;
    try {
        auto context = WiringContext{*this};
        auto changed = true;
        while (changed) {
            changed = drain_pending_wiring();
            changed = run_service_build_session() || changed;
            changed = drain_pending_wiring() || changed;
        }

        if (service_build_validate_) {
            service_build_validate_();
        }

        finalization_state_ = FinalizationState::finalized;
    } catch (...) {
        finalization_state_ = FinalizationState::failed;
        throw;
    }
}

bool GraphWiring::complete_pending_wiring() {
    if (finalization_state_ == FinalizationState::finalized) {
        throw WiringError{"a finalized GraphWiring cannot complete more construction"};
    }
    if (finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a failed GraphWiring cannot complete construction"};
    }
    if (finalization_state_ == FinalizationState::finalizing) {
        throw WiringError{"GraphWiring construction completion is already active"};
    }

    finalization_state_ = FinalizationState::finalizing;
    try {
        auto context = WiringContext{*this};
        const auto changed = drain_pending_wiring();
        finalization_state_ = FinalizationState::open;
        return changed;
    } catch (...) {
        finalization_state_ = FinalizationState::failed;
        throw;
    }
}

WiringPoint GraphWiring::complete_pending_wiring(WiringPoint result) {
    if (finalization_state_ == FinalizationState::finalized) {
        throw WiringError{"a finalized GraphWiring cannot complete more construction"};
    }
    if (finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a failed GraphWiring cannot complete construction"};
    }
    if (finalization_state_ == FinalizationState::finalizing) {
        throw WiringError{"GraphWiring construction completion is already active"};
    }
    if (!result.has_output() || &result.graph() != this) {
        throw WiringError{"a body result must belong to the GraphWiring being completed"};
    }

    finalization_state_ = FinalizationState::finalizing;
    try {
        auto context = WiringContext{*this};
        require_materialisation(result, result.type());
        auto concrete_result = WiringPoint{};
        auto changed = true;
        while (changed) {
            changed = drain_pending_wiring();
            concrete_result = materialize(result, result.type());
            changed = drain_pending_wiring() || changed;
        }
        finalization_state_ = FinalizationState::open;
        return concrete_result;
    } catch (...) {
        finalization_state_ = FinalizationState::failed;
        throw;
    }
}

bool GraphWiring::drain_pending_wiring() {
    const auto initial_node_count = nodes_.size();
    const auto initial_output_count = outputs_.size();
    const auto initial_materialized_count = materialized_wiring_points_.size();
    const auto initial_expanded_count = expanded_wiring_points_.size();
    const auto initial_pending_count = std::ranges::count_if(pending_node_calls_, [](const auto &call) {
        return call->state != PendingNodeCall::State::resolved;
    });

    auto requirement_index = std::size_t{};
    auto call_index = std::size_t{};
    while (requirement_index < materialisation_requirements_.size() || call_index < pending_node_calls_.size()) {
        while (requirement_index < materialisation_requirements_.size()) {
            const auto requirement = materialisation_requirements_[requirement_index++];
            const auto point = details::WiringRecord::point(*requirement.record);
            const auto &contexts = requirement.origins;
            auto diagnostics = details::WiringDiagnosticContext{*this, contexts};
            try {
                if (requirement.record->has_default_materialisation()) {
                    (void)wire_input(point, *requirement.type);
                } else {
                    (void)expand_materialisation(point, *requirement.type);
                }
            } catch (...) {
                throw WiringError::from_exception("materialisation failed", contexts, std::current_exception());
            }
        }
        if (call_index < pending_node_calls_.size()) {
            const auto identity = call_index++;
            try {
                (void)resolve_pending_node_call(identity);
            } catch (...) {
                const auto &call = *pending_node_calls_[identity];
                auto contexts = call.diagnostic_contexts;
                contexts.push_back({.stage = WiringFailureStage::callback,
                                    .path = call.path == nullptr ? std::string{} : call.path->text(),
                                    .operation = call.signature.name});
                throw WiringError::from_exception("pending node wiring failed", contexts, std::current_exception());
            }
        }
    }

    const auto remaining_pending_count = std::ranges::count_if(pending_node_calls_, [](const auto &call) {
        return call->state != PendingNodeCall::State::resolved;
    });
    return nodes_.size() != initial_node_count || outputs_.size() != initial_output_count ||
           materialized_wiring_points_.size() != initial_materialized_count ||
           expanded_wiring_points_.size() != initial_expanded_count || remaining_pending_count != initial_pending_count;
}

bool GraphWiring::run_service_build_session() {
    return service_build_ ? service_build_() : false;
}

bool GraphWiring::build_known_services() {
    if (finalization_state_ == FinalizationState::finalized || finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a closed GraphWiring cannot build services"};
    }
    return run_service_build_session();
}

void GraphWiring::finalize(const ServiceCatalogue &catalogue) {
    if (finalization_state_ == FinalizationState::finalized) {
        return;
    }
    if (finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a failed GraphWiring cannot be finalized"};
    }
    if (finalization_state_ == FinalizationState::finalizing) {
        throw WiringError{"GraphWiring finalization is already active"};
    }

    try {
        catalogue.build(*this);
        finalize();
    } catch (...) {
        if (finalization_state_ != FinalizationState::finalized) {
            finalization_state_ = FinalizationState::failed;
        }
        throw;
    }
}

std::shared_ptr<void> GraphWiring::service_build_session(const void *catalogue_identity) const {
    if (service_catalogue_identity_ != nullptr && service_catalogue_identity_ != catalogue_identity) {
        throw WiringError{"a GraphWiring cannot use more than one ServiceCatalogue"};
    }
    return service_build_session_;
}

void GraphWiring::attach_service_build_session(const void *catalogue_identity, std::shared_ptr<void> session,
                                              std::function<bool()> build, std::function<void()> validate) {
    if (finalization_state_ == FinalizationState::finalized || finalization_state_ == FinalizationState::failed) {
        throw WiringError{"a closed GraphWiring cannot attach a service build session"};
    }
    if (service_catalogue_identity_ != nullptr && service_catalogue_identity_ != catalogue_identity) {
        throw WiringError{"a GraphWiring cannot use more than one ServiceCatalogue"};
    }
    if (service_catalogue_identity_ == nullptr) {
        service_catalogue_identity_ = catalogue_identity;
        service_build_session_ = std::move(session);
        service_build_ = std::move(build);
        service_build_validate_ = std::move(validate);
        return;
    }
    if (service_build_session_ != session) {
        throw WiringError{"a GraphWiring already has a different service build session"};
    }
}

std::optional<WiringPoint> GraphWiring::find_wired_node(std::size_t hash, const NodeTemplate &node_template,
                                                        const ResolvedSignature &signature,
                                                        std::span<const WiredInput> inputs,
                                                        std::span<const ScalarType *const> state_types,
                                                        std::span<const WiringArgument> arguments) {
    const auto found = wired_nodes_.find(util::linear_hash_table::hash_code(hash), [&](auto position) {
        const auto &node = nodes_[position];
        return &node.node_template() == &node_template && same_resolved_signature(node.signature(), signature) &&
               std::ranges::equal(node.inputs(), inputs) && std::ranges::equal(node.state_types(), state_types) &&
               same_scalar_arguments(node.scalar_values(), arguments);
    });
    if (!found) {
        return std::nullopt;
    }

    return details::WiringRecord::point(*node_output_records_[*found]);
}

WiringPoint GraphWiring::add_inherited_requirement(details::NestedBindingIdentity identity, util::symbol name,
                                                   const TimeSeriesType &type, WiringPoint provider) {
    const auto requirement_index = inherited_requirements_.size();
    const auto requirement = InheritedTimeSeriesRequirement{std::move(identity), name, &type, provider};
    auto point =
        register_record(std::make_unique<details::InheritedTimeSeriesWiringRecord>(requirement.boundary_input_type()));
    inherited_requirements_.push_back(requirement);
    inherited_records_.push_back({point.record(), requirement_index});
    return point;
}

WiringPoint GraphWiring::inherited_requirement_point(std::size_t index) const {
    return details::WiringRecord::point(*inherited_records_.at(index).record);
}

WiringPoint GraphWiring::import_context(util::symbol name, WiringPoint provider) {
    if (&provider.graph() == this) {
        return provider;
    }
    if (parent_ == nullptr) {
        throw WiringError{"a context time series does not belong to an enclosing graph wiring"};
    }

    if (&provider.graph() != parent_) {
        provider = parent_->import_context(name, provider);
    }

    const auto source_provider = provider;
    const auto inherited = std::ranges::find(parent_->inherited_records_, provider.record(), &InheritedRecord::record);
    const auto *source_type = &provider.type();
    if (inherited != parent_->inherited_records_.end()) {
        source_type = parent_->inherited_requirements_[inherited->requirement].type;
    }

    const auto existing = std::ranges::find_if(context_bindings_, [&](const auto &requirement) {
        return requirement.name == name && requirement.type == source_type &&
               requirement.source_provider.record() == source_provider.record();
    });
    if (existing != context_bindings_.end()) {
        return existing->point;
    }

    if (inherited == parent_->inherited_records_.end()) {
        auto context = WiringContext{*parent_};
        provider = details::materialize_boundary_reference(source_provider);
    }

    auto point = add_inherited_requirement(details::NestedBindingIdentity::named(name), name, *source_type, provider);
    context_bindings_.push_back({name, source_type, source_provider, provider, point});
    return point;
}

WiredInput GraphWiring::wire_input(WiringPoint point, const TimeSeriesType &type) {
    if (&point.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto inherited = std::ranges::find(inherited_records_, point.record(), &InheritedRecord::record);
    if (inherited != inherited_records_.end()) {
        return WiredInput::inherited(inherited->requirement);
    }

    const auto materialized = materialize(point, type);
    return WiredInput::output(output_index(materialized));
}

WiredInput GraphWiring::resolve_inherited_input(std::size_t index) const {
    const auto &requirement = inherited_requirements_.at(index);
    if (!requirement.provider.has_output() || &requirement.provider.graph() != this) {
        return WiredInput::inherited(index);
    }

    return WiredInput::output(output_index(requirement.provider));
}

void GraphWiring::order_after_consumers(WiringPoint consumed, WiringPoint ordered_output) {
    ensure_open();
    if (!consumed.has_output() || &consumed.graph() != this || !ordered_output.has_output() ||
        &ordered_output.graph() != this) {
        throw WiringError{"an ordering constraint requires wiring points from the same GraphWiring"};
    }

    const auto existing = std::ranges::find_if(ordering_constraints_, [&](const auto &constraint) {
        return constraint.consumed.record() == consumed.record() &&
               constraint.ordered_output.record() == ordered_output.record();
    });
    if (existing != ordering_constraints_.end()) {
        return;
    }

    ordering_constraints_.push_back({consumed, ordered_output});
}

bool GraphWiring::input_consumes(WiredInput input, WiringPoint point) const {
    if (!point.has_output() || &point.record()->wiring() != this) {
        return false;
    }

    if (input.is_inherited()) {
        return input.index() < inherited_records_.size() && inherited_records_[input.index()].record == point.record();
    }

    if (input.index() >= output_records_.size()) {
        return false;
    }

    auto pending_records = std::vector<const details::WiringRecord *>{output_records_[input.index()]};
    auto visited_records = std::vector<const details::WiringRecord *>{};
    while (!pending_records.empty()) {
        const auto *input_record = pending_records.back();
        pending_records.pop_back();
        if (input_record == nullptr || std::ranges::find(visited_records, input_record) != visited_records.end()) {
            continue;
        }
        if (input_record == point.record()) {
            return true;
        }
        visited_records.push_back(input_record);
        for (const auto &materialization : materialized_wiring_points_) {
            if (materialization.result == input_record) {
                pending_records.push_back(materialization.record);
            }
        }
    }

    return false;
}

void GraphWiring::cache_wired_node(std::size_t hash, std::size_t node_index) {
    wired_nodes_.insert(util::linear_hash_table::hash_code(hash), static_cast<std::uint32_t>(node_index),
                        fixed_memory_);
}

std::size_t GraphWiring::register_node(WiredNode node) {
    ensure_open();
    const auto index = nodes_.size();
    nodes_.reserve(index + 1);
    node_output_records_.reserve(index + 1);

    nodes_.push_back(std::move(node));
    node_output_records_.push_back(nullptr);
    if (!active_pending_node_calls_.empty()) {
        auto &call = *pending_node_calls_[active_pending_node_calls_.back()];
        call.concrete_node_index = index;
    }
    return index;
}

void GraphWiring::register_node_output(std::size_t node_index, WiringPoint output, std::optional<std::size_t> hash) {
    ensure_open();
    node_output_records_[node_index] = output.record();
    if (hash) {
        cache_wired_node(*hash, node_index);
    }
}

WiringPoint GraphWiring::register_record(std::unique_ptr<details::WiringRecord> record) {
    ensure_open();
    auto &result = *record;
    result.initialize(*this);
    wiring_records_.push_back(std::move(record));
    return details::WiringRecord::point(result);
}

WiringPoint GraphWiring::register_item_record(WiringPoint parent, item_id_t item,
                                              std::unique_ptr<details::WiringRecord> record) {
    ensure_open();
    if (&parent.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto existing = std::ranges::find_if(
        item_records_, [&](const auto &entry) { return entry.parent == parent.record() && entry.item == item; });
    if (existing != item_records_.end()) {
        return details::WiringRecord::point(*existing->result);
    }

    item_records_.reserve(item_records_.size() + 1);
    auto result = register_record(std::move(record));
    item_records_.push_back({parent.record(), item, result.record()});
    return result;
}

WiringPoint GraphWiring::register_output(WiredOutput output, std::unique_ptr<details::WiringRecord> record) {
    ensure_open();
    wiring_records_.reserve(wiring_records_.size() + 1);
    outputs_.reserve(outputs_.size() + 1);
    output_records_.reserve(output_records_.size() + 1);

    auto &result = *record;
    const auto output_index = outputs_.size();
    if (output.parent != WiredOutput::npos) {
        output_items_.insert(output_item_hash(output.parent, output.item), static_cast<std::uint32_t>(output_index),
                             fixed_memory_);
    }

    result.initialize(*this, output_index);

    outputs_.push_back(output);
    output_records_.push_back(&result);
    wiring_records_.push_back(std::move(record));
    return details::WiringRecord::point(result);
}

std::optional<WiringPoint> GraphWiring::find_output_item(std::size_t parent, item_id_t item) const {
    const auto found = output_items_.find(output_item_hash(parent, item), [&](auto position) {
        const auto &output = outputs_[position];
        return output.parent == parent && output.item == item;
    });
    if (!found) {
        return std::nullopt;
    }

    return details::WiringRecord::point(*output_records_[*found]);
}

std::size_t GraphWiring::output_index(WiringPoint point) const {
    if (&point.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    if (!point.record()->has_wired_output()) {
        throw WiringError{"a WiringPoint has no wired output until it is consumed"};
    }

    return point.record()->wired_output_index();
}

} // namespace bg

namespace bg {

WiringPoint GraphWiring::require_inherited_time_series(details::NestedBindingIdentity identity, util::symbol name,
                                                       const TimeSeriesType &type) {
    ensure_open();
    const auto existing = std::ranges::find_if(inherited_requirements_, [&](const auto &requirement) {
        return requirement.identity == identity && requirement.type == &type;
    });
    if (existing != inherited_requirements_.end()) {
        const auto index = static_cast<std::size_t>(existing - inherited_requirements_.begin());
        return inherited_requirement_point(index);
    }

    auto provider = WiringPoint{};
    if (parent_ != nullptr) {
        provider = parent_->require_inherited_time_series(identity, name, type);
    }

    return add_inherited_requirement(std::move(identity), name, type, provider);
}

void GraphWiring::provide_inherited_time_series(const details::NestedBindingIdentity &identity,
                                                const TimeSeriesType &type, WiringPoint provider) {
    ensure_open();
    if (parent_ != nullptr) {
        throw WiringError{"an inherited time series can only be provided on the root GraphWiring"};
    }
    if (!provider.has_output() || &provider.graph() != this) {
        throw WiringError{"an inherited time series provider must belong to the root GraphWiring"};
    }
    if (!identity.is_object()) {
        throw WiringError{"only an object-identified inherited requirement can be provided later"};
    }

    const auto found = std::ranges::find_if(inherited_requirements_, [&](const auto &requirement) {
        return requirement.identity == identity && requirement.type == &type;
    });
    if (found == inherited_requirements_.end()) {
        throw WiringError{"the inherited time series requirement was not found"};
    }
    if (found->provider.has_output()) {
        throw WiringError{"the inherited time series requirement already has a provider"};
    }

    if (!found->boundary_input_type().accepts(provider.type())) {
        throw WiringError{"the inherited time series provider has an incompatible type"};
    }

    auto context = WiringContext{*this};
    auto reference = details::materialize_boundary_reference(provider);
    found->provider = reference;
}

std::size_t GraphWiring::require_service_binding(details::NestedBindingIdentity identity,
                                                 ServiceBindingOrdering ordering) {
    const auto existing =
        std::ranges::find(service_binding_requirements_, identity, &ServiceBindingRequirement::identity);
    if (existing != service_binding_requirements_.end()) {
        if (existing->ordering != ordering) {
            throw WiringError{"a service binding has conflicting ordering requirements"};
        }
        return static_cast<std::size_t>(existing - service_binding_requirements_.begin());
    }

    const auto parent_requirement =
        parent_ == nullptr ? std::optional<std::size_t>{} : parent_->require_service_binding(identity, ordering);
    service_binding_requirements_.push_back({std::move(identity), parent_requirement, {}, {}, ordering});
    return service_binding_requirements_.size() - 1;
}

void GraphWiring::provide_service_binding(const details::NestedBindingIdentity &identity, WiringPoint provider) {
    if (parent_ != nullptr) {
        throw WiringError{"a service binding can only be provided on the root GraphWiring"};
    }
    if (!provider.has_output() || &provider.graph() != this) {
        throw WiringError{"a service binding provider must belong to the root GraphWiring"};
    }

    const auto requirement =
        std::ranges::find(service_binding_requirements_, identity, &ServiceBindingRequirement::identity);
    if (requirement == service_binding_requirements_.end()) {
        throw WiringError{"the service binding requirement was not found"};
    }
    if (requirement->provider.has_output()) {
        throw WiringError{"the service binding requirement already has a provider"};
    }

    requirement->provider = provider;
}

std::size_t GraphWiring::node_index(NodeCallIdentity identity) const {
    if (identity.value >= pending_node_calls_.size()) {
        throw WiringError{"a service binding consumer is not a node in this GraphWiring"};
    }
    const auto &call = *pending_node_calls_[identity.value];
    if (!call.concrete_node_index) {
        throw WiringError{"a node call has not been bound to a concrete node"};
    }
    return *call.concrete_node_index;
}

std::optional<NodeCallIdentity> GraphWiring::node_call_identity(WiringPoint point) const noexcept {
    if (!point.has_output() || &point.record()->wiring() != this) {
        return std::nullopt;
    }
    return point.record()->node_call_identity();
}

void GraphWiring::register_service_binding_consumer(const details::NestedBindingIdentity &identity,
                                                    NodeCallIdentity consumer) {
    ensure_open();
    if (consumer.value >= pending_node_calls_.size()) {
        throw WiringError{"a service binding consumer is not a node in this GraphWiring"};
    }

    const auto requirement =
        std::ranges::find(service_binding_requirements_, identity, &ServiceBindingRequirement::identity);
    if (requirement == service_binding_requirements_.end()) {
        throw WiringError{"the service binding requirement was not found"};
    }
    if (!std::ranges::contains(requirement->consumers, consumer)) {
        requirement->consumers.push_back(consumer);
    }
}

void GraphWiring::register_nested_service_consumers(const GraphWiring &nested, NodeCallIdentity consumer) {
    ensure_open();
    if (nested.parent_ != this) {
        throw WiringError{"nested service consumers must belong to a child GraphWiring"};
    }
    if (consumer.value >= pending_node_calls_.size()) {
        throw WiringError{"a nested service consumer is not a node in this GraphWiring"};
    }

    for (const auto &nested_requirement : nested.service_binding_requirements_) {
        if (!nested_requirement.parent_requirement ||
            *nested_requirement.parent_requirement >= service_binding_requirements_.size()) {
            throw WiringError{"a nested service requirement has an invalid parent requirement"};
        }

        auto &requirement = service_binding_requirements_[*nested_requirement.parent_requirement];
        if (requirement.identity != nested_requirement.identity) {
            throw WiringError{"a nested service requirement does not match its parent requirement"};
        }
        if (!std::ranges::contains(requirement.consumers, consumer)) {
            requirement.consumers.push_back(consumer);
        }
    }
}

std::size_t GraphWiring::output_node_index(WiringPoint point) const {
    return outputs_[output_index(point)].node_index;
}

} // namespace bg

#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/graph_signature.hpp>
#include <bg/graph/details/wiring_callable.hpp>
#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/node_call_identity.hpp>
#include <bg/graph/wired_input.hpp>
#include <bg/graph/wired_node.hpp>
#include <bg/graph/wired_output.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_path.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/linear_hash_table.hpp>

#include <cstdint>
#include <functional>
#include <deque>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bg {

class NodeTemplate;
class Operator;
class ReferenceService;
class Service;
class ServiceDefinition;
class ServiceCatalogue;
class SubscriptionService;
class Signature;
class TimeSeriesRuntimeType;
class TimeSeriesType;
class TypeMap;
class WiringTemplate;

namespace details {
class WiringRecord;
class WiringArgumentDescriptor;
class GraphWiringStructure;
class LateBoundWiringRecord;
class ContextScope;
class ServiceRequirements;
struct OperatorResolutionCache;
WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item, std::unique_ptr<WiringRecord> record);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const WiringPoint>,
                                            std::optional<WiringPoint>);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const std::size_t>,
                                            std::optional<std::size_t>);
} // namespace details

class GraphWiring {
  public:
    struct NodeCallResult {
        NodeCallIdentity identity;
        std::optional<WiringPoint> output;
    };

    enum class ServiceBindingOrdering : std::uint8_t {
        provider_before_consumers,
        provider_after_consumers,
    };

    struct InheritedTimeSeriesRequirement {
        details::NestedBindingIdentity identity;
        util::symbol name;
        const TimeSeriesType *type;
        WiringPoint provider;
        WiringPoint consumer_point;

        const TimeSeriesType &boundary_input_type() const;
    };

    struct OrderingConstraint {
        WiringPoint consumed;
        WiringPoint ordered_output;
    };

    struct ServiceBindingRequirement {
        details::NestedBindingIdentity identity;
        std::optional<std::size_t> parent_requirement;
        WiringPoint provider;
        std::vector<NodeCallIdentity> consumers;
        ServiceBindingOrdering ordering{ServiceBindingOrdering::provider_before_consumers};
    };

    GraphWiring();
    explicit GraphWiring(allocator memory);
    explicit GraphWiring(GraphWiring &parent);
    GraphWiring(GraphWiring &parent, allocator memory);
    ~GraphWiring();

    GraphWiring(const GraphWiring &) = delete;
    GraphWiring &operator=(const GraphWiring &) = delete;
    GraphWiring(GraphWiring &&) = delete;
    GraphWiring &operator=(GraphWiring &&) = delete;

    std::span<const WiredNode> nodes() const noexcept { return nodes_; }

    std::span<const WiredOutput> outputs() const noexcept { return outputs_; }

    std::span<const InheritedTimeSeriesRequirement> inherited_requirements() const noexcept {
        return inherited_requirements_;
    }

    std::span<const OrderingConstraint> ordering_constraints() const noexcept { return ordering_constraints_; }

    std::span<const ServiceBindingRequirement> service_binding_requirements() const noexcept {
        return service_binding_requirements_;
    }

    // Resolve collected node calls and materialisations into concrete wired nodes.
    void finalize();
    void finalize(const ServiceCatalogue &catalogue);
    // Engine operation: resolve collected calls and materialisations while allowing later wiring to continue.
    [[nodiscard]] bool complete_pending_wiring();
    // Completes wiring needed by a declared body result and returns its concrete form while the graph remains open.
    [[nodiscard]] WiringPoint complete_pending_wiring(WiringPoint result);
    // Engine operation: wire currently known services using the attached catalogue session.
    [[nodiscard]] bool build_known_services();
    [[nodiscard]] bool is_finalized() const noexcept { return finalization_state_ == FinalizationState::finalized; }
    std::shared_ptr<void> service_build_session(const void *catalogue_identity) const;
    void attach_service_build_session(const void *catalogue_identity, std::shared_ptr<void> session,
                                     std::function<bool()> build, std::function<void()> validate);

    [[nodiscard]] WiringPoint require_inherited_time_series(details::NestedBindingIdentity identity, util::symbol name,
                                                            const TimeSeriesType &type);
    void provide_inherited_time_series(const details::NestedBindingIdentity &identity, const TimeSeriesType &type,
                                       WiringPoint provider);
    [[nodiscard]] WiredInput resolve_inherited_input(WiredInput input) const;
    void order_after_consumers(WiringPoint consumed, WiringPoint ordered_output);
    bool input_consumes(WiredInput input, WiringPoint point) const;
    void register_service_binding_consumer(const details::NestedBindingIdentity &identity, NodeCallIdentity consumer);
    void register_nested_service_consumers(const GraphWiring &nested, NodeCallIdentity consumer);

    [[nodiscard]] const NodeTemplate &own_node_template(std::unique_ptr<NodeTemplate> node_template);

    [[nodiscard]] std::optional<WiringPoint> invoke(const WiringTemplate &wiring_template,
                                                    std::span<const WiringArgument> arguments,
                                                    const TypeMap *seed_type_map = nullptr);
    [[nodiscard]] NodeCallResult invoke_node(const NodeTemplate &node_template,
                                            std::span<const WiringArgument> arguments);
    [[nodiscard]] std::optional<WiringPoint> invoke_resolved(const WiringTemplate &wiring_template,
                                                             SignatureResolution resolution,
                                                             std::span<const WiringArgument> arguments,
                                                             std::span<const details::WiringArgumentDescriptor> descriptors,
                                                             util::symbol path_name);
    [[nodiscard]] WiringPoint defer_node_call(const NodeTemplate &node_template, ResolvedSignature signature,
                                              const TypeMap &type_map,
                                              std::span<const WiringArgument> arguments,
                                              const TimeSeriesRuntimeType *output_runtime_type);
    [[nodiscard]] std::size_t node_index(NodeCallIdentity identity) const;
    [[nodiscard]] std::optional<NodeCallIdentity> node_call_identity(WiringPoint point) const noexcept;
    bool pending_node_call_ready(std::size_t identity) const noexcept;
    [[nodiscard]] WiringPoint resolve_pending_node_call(std::size_t identity);
    [[nodiscard]] std::size_t register_node(WiredNode node);
    void register_node_output(std::size_t node_index, WiringPoint output, std::optional<std::size_t> hash);
    [[nodiscard]] WiringPoint register_record(std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] WiringPoint register_item_record(WiringPoint parent, item_id_t item,
                                                   std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] WiringPoint register_output(WiredOutput output);
    [[nodiscard]] WiringPoint register_output(WiredOutput output, std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] WiringPoint borrowed_input(const TimeSeriesType &type);
    // Creates a typed deferred point that must be provided before compilation.
    [[nodiscard]] WiringPoint late_bound_wiring(const TimeSeriesType &type);
    // Binds a deferred point once to a compatible provider in this GraphWiring.
    void provide_late_bound_wiring(WiringPoint requirement, WiringPoint provider);
    [[nodiscard]] std::span<const TimeSeriesType *const> borrowed_input_types() const noexcept {
        return borrowed_input_types_;
    }
    // Extension-facing creation and expansion for deferred wiring points.
    template <typename TimeSeries, typename... Arguments>
    [[nodiscard]] wp<TimeSeries> create_wp(Arguments &&...arguments);
    [[nodiscard]] WiringPoint create_wp(const TimeSeriesType &semantic_type,
                                        std::span<const WiringArgument> arguments);
    void add_materialisation_option(WiringPoint point, const TimeSeriesType &target_type, double cost,
                                   Signature input_signature, WiringMaterialisationBuilder builder);
    std::vector<WiringMaterialisationOption> materialisation_options(WiringPoint point);
    // Preserves a selected advertised form as a lazy point for concrete consumers.
    [[nodiscard]] WiringPoint materialisation_form(WiringPoint point, const TimeSeriesType &target_type);
    // Registers a root demand and prepares every concrete callback input before any builder runs.
    void require_materialisation(WiringPoint point, const TimeSeriesType &target_type);
    void require_materialisation(WiringPoint point, const TimeSeriesType &target_type,
                                 WiringRequestOrigin origin);
    // Reports whether this source identity and target were included in a registered root's prepared inputs.
    bool materialisation_required(WiringPoint point, const TimeSeriesType &target_type) const;
    // Runs the selected builder synchronously. Callers must activate this graph's WiringContext if the
    // builder wires graph operations. The returned point may remain lazy.
    [[nodiscard]] WiringPoint expand_materialisation(WiringPoint point, const TimeSeriesType &target_type);
    [[nodiscard]] WiringPoint materialize(WiringPoint point, const TimeSeriesType &type);
    [[nodiscard]] WiringPoint output_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type);
    [[nodiscard]] std::size_t output_index(WiringPoint point) const;
    [[nodiscard]] std::size_t output_node_index(WiringPoint point) const;
    [[nodiscard]] std::optional<WiringPoint> find_output_item(std::size_t parent, item_id_t item) const;
    [[nodiscard]] std::optional<WiringPoint> find_wired_node(std::size_t hash, const NodeTemplate &node_template,
                                                             const ResolvedSignature &signature,
                                                             std::span<const WiredInput> inputs,
                                                             std::span<const ScalarType *const> state_types,
                                                             std::span<const WiringArgument> arguments);
    void cache_wired_node(std::size_t hash, std::size_t node_index);
    [[nodiscard]] WiringPoint import_context(util::symbol name, WiringPoint provider);
    [[nodiscard]] WiringPoint add_inherited_requirement(details::NestedBindingIdentity identity, util::symbol name,
                                                        const TimeSeriesType &type, WiringPoint provider);
    [[nodiscard]] WiringPoint inherited_requirement_point(std::size_t index) const;
    [[nodiscard]] WiredInput wire_input(WiringPoint point, const TimeSeriesType &type);
    [[nodiscard]] std::optional<WiringPoint> require_service(const ServiceDefinition &service, ResolvedSignature signature,
                                                             const TypeMap &type_map,
                                                             std::span<const WiringArgument> arguments);
    std::size_t
    require_service_binding(details::NestedBindingIdentity identity,
                            ServiceBindingOrdering ordering = ServiceBindingOrdering::provider_before_consumers);
    void provide_service_binding(const details::NestedBindingIdentity &identity, WiringPoint provider);
    details::ServiceRequirements &root_service_requirements();
    details::OperatorResolutionCache &operator_resolution_cache(const Operator &operation);

    [[nodiscard]] const WiringPath &make_path(util::symbol name, util::symbol label);
    util::symbol default_label() const;

    void destroy_scalars(std::span<const WiredScalar> scalars) noexcept;
    allocator &allocation() noexcept { return allocator_; }
    void *allocate_fixed(std::size_t size, std::size_t alignment) {
        return fixed_memory_.allocate(size, alignment);
    }
    void deallocate_fixed(void *data, std::size_t size, std::size_t alignment) noexcept {
        fixed_memory_.deallocate(data, size, alignment);
    }
    allocator &variable_allocator() noexcept;
    allocator &variable_allocator(const ScalarRuntimeType &runtime_type);
    bool is_root() const noexcept { return parent_ == nullptr; }
    details::ServiceRequirements *service_requirements() noexcept { return service_requirements_.get(); }

  private:
    allocator allocator_;
    GraphWiring *parent_{};
    util::hash_dictionary<const Operator *, std::unique_ptr<details::OperatorResolutionCache>> operator_caches_;
    std::pmr::unsynchronized_pool_resource fixed_memory_;
    std::optional<std::pmr::unsynchronized_pool_resource> variable_memory_;
    allocator variable_allocator_;
    std::vector<std::unique_ptr<NodeTemplate>> node_templates_;
    struct PendingNodeCall;
    std::vector<std::unique_ptr<PendingNodeCall>> pending_node_calls_;
    std::vector<std::size_t> active_pending_node_calls_;
    struct NodeCallCapture {
        const NodeTemplate *node_template;
        std::optional<std::size_t> identity;
    };
    std::vector<NodeCallCapture *> node_call_captures_;
    enum class FinalizationState : std::uint8_t { open, finalizing, finalized, failed };
    FinalizationState finalization_state_{FinalizationState::open};
    void ensure_open() const;
    [[nodiscard]] WiringPoint import_context_binding(util::symbol name, WiringPoint provider, bool consume);
    [[nodiscard]] WiringPoint require_inherited_binding(details::NestedBindingIdentity identity, util::symbol name,
                                                        const TimeSeriesType &type, bool consume);
    bool drain_pending_wiring();
    bool run_service_build_session();
    const void *service_catalogue_identity_{};
    std::shared_ptr<void> service_build_session_;
    std::function<bool()> service_build_;
    std::function<void()> service_build_validate_;
    std::unique_ptr<details::ServiceRequirements> service_requirements_;
    std::vector<std::unique_ptr<details::WiringRecord>> wiring_records_;
    std::vector<details::LateBoundWiringRecord *> late_bound_wiring_records_;
    struct ItemRecord {
        const details::WiringRecord *parent;
        item_id_t item;
        const details::WiringRecord *result;
    };
    std::vector<ItemRecord> item_records_;
    std::vector<WiredNode> nodes_;
    std::vector<WiredOutput> outputs_;
    std::vector<const details::WiringRecord *> output_records_;
    std::vector<const details::WiringRecord *> node_output_records_;
    std::vector<const TimeSeriesType *> borrowed_input_types_;
    util::linear_hash_table wired_nodes_;
    util::linear_hash_table output_items_;
    struct MaterializedWiring {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
        const details::WiringRecord *result;
    };
    std::vector<MaterializedWiring> materialized_wiring_points_;
    struct SelectedMaterialisationForm {
        const details::WiringRecord *source;
        const TimeSeriesType *type;
        const details::WiringRecord *result;
    };
    std::vector<SelectedMaterialisationForm> selected_materialisation_forms_;
    struct ExpandedWiring {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
        const details::WiringRecord *result;
    };
    std::vector<ExpandedWiring> expanded_wiring_points_;
    struct MaterialisationRequest {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
    };
    std::vector<MaterialisationRequest> active_materialisations_;
    std::vector<MaterialisationRequest> active_materialisation_expansions_;
    struct MaterialisationRequirement {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
        std::vector<WiringErrorContext> origins;
    };
    struct PreparedMaterialisation {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
        std::vector<WiringArgument> arguments;
        std::vector<const TimeSeriesType *> input_targets;
        std::vector<details::MaterialisationDependency> dependencies;
    };
    std::vector<MaterialisationRequirement> materialisation_requirements_;
    std::vector<PreparedMaterialisation> prepared_materialisations_;
    struct InheritedRecord {
        const details::WiringRecord *record;
        std::size_t requirement;
    };
    struct ContextBinding {
        util::symbol name;
        const TimeSeriesType *type;
        WiringPoint source_provider;
        WiringPoint provider;
        WiringPoint transport;
        WiringPoint point;
    };
    std::vector<InheritedTimeSeriesRequirement> inherited_requirements_;
    std::vector<OrderingConstraint> ordering_constraints_;
    std::vector<ServiceBindingRequirement> service_binding_requirements_;
    std::vector<ContextBinding> context_bindings_;
    std::vector<InheritedRecord> inherited_records_;
    std::pmr::deque<WiringPath> paths_;
};

template <typename TimeSeries, typename... Arguments>
wp<TimeSeries> GraphWiring::create_wp(Arguments &&...arguments) {
    auto retained_arguments = details::make_wiring_arguments(std::forward<Arguments>(arguments)...);
    return create_wp(TimeSeries::type(), retained_arguments).template as<TimeSeries>();
}

template <typename TimeSeries, auto... Properties>
template <typename Target, typename Builder>
void wp<TimeSeries, Properties...>::materialisation_option(Builder &&builder, double cost) const {
    point_.template materialisation_option<Target>(std::forward<Builder>(builder), cost);
}

template <typename Target, typename Builder>
void WiringPoint::materialisation_option(Builder &&builder, double cost) const {
    using Function = std::remove_cvref_t<Builder>;
    auto input_signature = details::make_graph_input_signature<Function>(util::symbol{"materialisation"});
    auto erased_builder = [builder = std::forward<Builder>(builder)](
                              std::span<const WiringArgument> arguments) -> WiringPoint {
        return details::invoke_graph_function(builder, arguments);
    };
    add_materialisation_option(Target::type(), cost, std::move(input_signature),
                               WiringMaterialisationBuilder{std::move(erased_builder)});
}

} // namespace bg

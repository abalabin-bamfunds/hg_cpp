#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/nested_binding_identity.hpp>
#include <bg/graph/wired_input.hpp>
#include <bg/graph/wired_node.hpp>
#include <bg/graph/wired_output.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_path.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/util/linear_hash_table.hpp>

#include <cstdint>
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
class TimeSeriesRuntimeType;
class TimeSeriesType;
class WiringTemplate;

namespace details {
class WiringRecord;
class GraphWiringStructure;
class ContextScope;
class ServiceRequirements;
WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item, std::unique_ptr<WiringRecord> record);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const WiringPoint>,
                                            std::optional<WiringPoint>);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const std::size_t>,
                                            std::optional<std::size_t>);
} // namespace details

class GraphWiring {
  public:
    enum class ServiceBindingOrdering : std::uint8_t {
        provider_before_consumers,
        provider_after_consumers,
    };

    struct InheritedTimeSeriesRequirement {
        details::NestedBindingIdentity identity;
        util::symbol name;
        const TimeSeriesType *type;
        WiringPoint provider;

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
        std::vector<std::size_t> consumers;
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

    [[nodiscard]] WiringPoint require_inherited_time_series(details::NestedBindingIdentity identity, util::symbol name,
                                                            const TimeSeriesType &type);
    void provide_inherited_time_series(const details::NestedBindingIdentity &identity, const TimeSeriesType &type,
                                       WiringPoint provider);
    [[nodiscard]] WiredInput resolve_inherited_input(std::size_t requirement) const;
    void order_after_consumers(WiringPoint consumed, WiringPoint ordered_output);
    bool input_consumes(WiredInput input, WiringPoint point) const noexcept;
    void register_service_binding_consumer(const details::NestedBindingIdentity &identity, std::size_t node_index);
    void register_nested_service_consumers(const GraphWiring &nested, std::size_t node_index);

    [[nodiscard]] const NodeTemplate &own_node_template(std::unique_ptr<NodeTemplate> node_template);

    [[nodiscard]] std::optional<WiringPoint> invoke(const WiringTemplate &wiring_template,
                                                    std::span<const WiringArgument> arguments,
                                                    const TypeMap *seed_type_map = nullptr);
    [[nodiscard]] std::optional<WiringPoint> invoke_resolved(const WiringTemplate &wiring_template,
                                                             SignatureResolution resolution,
                                                             std::span<const WiringArgument> arguments,
                                                             util::symbol path_name);
    [[nodiscard]] std::size_t register_node(WiredNode node);
    void register_node_output(std::size_t node_index, WiringPoint output, std::optional<std::size_t> hash);
    [[nodiscard]] WiringPoint register_record(std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] WiringPoint register_item_record(WiringPoint parent, item_id_t item,
                                                   std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] WiringPoint register_output(WiredOutput output);
    [[nodiscard]] WiringPoint register_output(WiredOutput output, std::unique_ptr<details::WiringRecord> record);
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
    std::pmr::unsynchronized_pool_resource fixed_memory_;
    std::optional<std::pmr::unsynchronized_pool_resource> variable_memory_;
    allocator variable_allocator_;
    std::vector<std::unique_ptr<NodeTemplate>> node_templates_;
    std::unique_ptr<details::ServiceRequirements> service_requirements_;
    std::vector<std::unique_ptr<details::WiringRecord>> wiring_records_;
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
    util::linear_hash_table wired_nodes_;
    util::linear_hash_table output_items_;
    struct MaterializedWiring {
        const details::WiringRecord *record;
        const TimeSeriesType *type;
        const details::WiringRecord *result;
    };
    std::vector<MaterializedWiring> materialized_wiring_points_;
    struct InheritedRecord {
        const details::WiringRecord *record;
        std::size_t requirement;
    };
    struct ContextBinding {
        util::symbol name;
        const TimeSeriesType *type;
        WiringPoint provider;
        WiringPoint point;
    };
    std::vector<InheritedTimeSeriesRequirement> inherited_requirements_;
    std::vector<OrderingConstraint> ordering_constraints_;
    std::vector<ServiceBindingRequirement> service_binding_requirements_;
    std::vector<ContextBinding> context_bindings_;
    std::vector<InheritedRecord> inherited_records_;
    std::pmr::deque<WiringPath> paths_;
};

} // namespace bg

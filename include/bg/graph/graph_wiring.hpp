#pragma once

#include <bg/allocator.hpp>
#include <bg/graph/details/inherited_time_series_identity.hpp>
#include <bg/graph/wired_input.hpp>
#include <bg/graph/wired_node.hpp>
#include <bg/graph/wired_output.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_path.hpp>
#include <bg/graph/wiring_point.hpp>
#include <bg/util/linear_hash_table.hpp>

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
class Service;
class ServiceCatalogue;
class TimeSeriesRuntimeType;
class TimeSeriesType;
class WiringTemplate;

namespace details {
class WiringRecord;
class GraphWiringStructure;
class ContextScope;
class ServicePlan;
WiringPoint add_wiring_record(std::unique_ptr<WiringRecord> record);
WiringPoint add_item_wiring_record(WiringPoint parent, item_id_t item, std::unique_ptr<WiringRecord> record);
WiringPoint materialize_wiring_point(WiringPoint point, const TimeSeriesType &type);
WiringPoint add_wired_output(GraphWiring &wiring, WiredOutput output);
void set_wired_output_runtime_type(WiringPoint output, const TimeSeriesRuntimeType &runtime_type);
WiringPoint wiring_point_for_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type);
std::size_t wired_output_node_index(const GraphWiring &wiring, WiringPoint point);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const WiringPoint>,
                                            std::optional<WiringPoint>);
GraphWiringStructure graph_wiring_structure(const GraphWiring &, std::span<const std::size_t>,
                                            std::optional<std::size_t>);
} // namespace details

class GraphWiring {
  public:
    struct InheritedTimeSeriesRequirement {
        details::InheritedTimeSeriesIdentity identity;
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
        const void *identity;
        std::size_t parent_slot;
        WiringPoint provider;
        std::vector<std::size_t> consumers;
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

    [[nodiscard]] WiringPoint require_inherited_time_series(details::InheritedTimeSeriesIdentity identity,
                                                            util::symbol name, const TimeSeriesType &type);
    void provide_inherited_time_series(const details::InheritedTimeSeriesIdentity &identity, const TimeSeriesType &type,
                                       WiringPoint provider);
    [[nodiscard]] WiredInput resolve_inherited_input(std::size_t requirement) const;
    void order_after_consumers(WiringPoint consumed, WiringPoint ordered_output);
    bool input_consumes(WiredInput input, WiringPoint point) const noexcept;
    void register_nested_service_consumers(const GraphWiring &nested, std::size_t node_index);

    [[nodiscard]] const NodeTemplate &own_node_template(std::unique_ptr<NodeTemplate> node_template);

  private:
    friend class NodeTemplate;
    friend class Operator;
    friend class Service;
    friend class ServiceCatalogue;
    friend class WiringPoint;
    friend class WiringTemplate;
    friend WiringPoint details::add_wiring_record(std::unique_ptr<details::WiringRecord> record);
    friend WiringPoint details::add_item_wiring_record(WiringPoint parent, item_id_t item,
                                                       std::unique_ptr<details::WiringRecord> record);
    friend WiringPoint details::materialize_wiring_point(WiringPoint point, const TimeSeriesType &type);
    friend WiringPoint details::add_wired_output(GraphWiring &wiring, WiredOutput output);
    friend void details::set_wired_output_runtime_type(WiringPoint output, const TimeSeriesRuntimeType &runtime_type);
    friend WiringPoint details::wiring_point_for_item(WiringPoint parent, item_id_t item, const TimeSeriesType &type);
    friend std::size_t details::wired_output_node_index(const GraphWiring &wiring, WiringPoint point);
    friend details::GraphWiringStructure
    details::graph_wiring_structure(const GraphWiring &, std::span<const WiringPoint>, std::optional<WiringPoint>);
    friend details::GraphWiringStructure
    details::graph_wiring_structure(const GraphWiring &, std::span<const std::size_t>, std::optional<std::size_t>);
    friend class details::ContextScope;
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
    [[nodiscard]] WiringPoint register_output(WiredOutput output, std::unique_ptr<details::WiringRecord> record);
    [[nodiscard]] std::size_t output_index(WiringPoint point) const;
    [[nodiscard]] std::optional<WiringPoint> find_output_item(std::size_t parent, item_id_t item) const;
    [[nodiscard]] std::optional<WiringPoint> find_wired_node(std::size_t hash, const NodeTemplate &node_template,
                                                             const ResolvedSignature &signature,
                                                             std::span<const WiredInput> inputs,
                                                             std::span<const ScalarType *const> state_types,
                                                             std::span<const WiringArgument> arguments);
    void cache_wired_node(std::size_t hash, std::size_t node_index);
    [[nodiscard]] WiringPoint import_context(util::symbol name, WiringPoint provider);
    [[nodiscard]] WiringPoint add_inherited_requirement(details::InheritedTimeSeriesIdentity identity,
                                                        util::symbol name, const TimeSeriesType &type,
                                                        WiringPoint provider);
    [[nodiscard]] WiringPoint inherited_requirement_point(std::size_t index) const;
    [[nodiscard]] WiredInput wire_input(WiringPoint point, const TimeSeriesType &type);
    [[nodiscard]] std::optional<WiringPoint> require_service(const Service &service, ResolvedSignature signature,
                                                             const TypeMap &type_map,
                                                             std::span<const WiringArgument> arguments);
    std::size_t require_service_binding(const void *identity);
    void provide_service_binding(const void *identity, WiringPoint provider);
    details::ServicePlan &root_service_plan();

    [[nodiscard]] const WiringPath &make_path(util::symbol name, util::symbol label);
    util::symbol default_label() const;

    void destroy_scalars(std::span<const WiredScalar> scalars) noexcept;
    allocator &variable_allocator() noexcept;
    allocator &variable_allocator(const ScalarRuntimeType &runtime_type);

    allocator allocator_;
    GraphWiring *parent_{};
    std::pmr::unsynchronized_pool_resource fixed_memory_;
    std::optional<std::pmr::unsynchronized_pool_resource> variable_memory_;
    allocator variable_allocator_;
    std::vector<std::unique_ptr<NodeTemplate>> node_templates_;
    std::unique_ptr<details::ServicePlan> service_plan_;
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

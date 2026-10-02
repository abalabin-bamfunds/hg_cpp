#include <bg/graph/graph_wiring.hpp>

#include <bg/graph/details/service_requirements.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include "time_series/ref/ref_node.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace bg {

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

std::optional<WiringPoint> GraphWiring::invoke_resolved(const WiringTemplate &wiring_template,
                                                        SignatureResolution resolution,
                                                        std::span<const WiringArgument> arguments,
                                                        util::symbol path_name) {
    auto signature = std::move(resolution.signature);

    const auto *expected_output = signature.output;
    const auto label = wiring_template.label(signature, arguments, default_label());
    const auto &path = make_path(path_name, label);
    auto context = WiringContext{*this, path};

    auto output = wiring_template.wire(*this, std::move(signature), resolution.type_map, arguments);

    if ((expected_output == nullptr) != !output.has_value()) {
        throw WiringError{"a wiring-template result does not match its output declaration"};
    }
    if (!output) {
        return std::nullopt;
    }
    if (&output->graph() != this) {
        throw WiringError{"a wiring-template returned a WiringPoint from another GraphWiring"};
    }

    if (!expected_output->accepts(output->type())) {
        throw WiringError{"a wiring-template result does not satisfy its resolved output type"};
    }

    return output;
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

    const auto inherited = std::ranges::find(parent_->inherited_records_, provider.record(), &InheritedRecord::record);
    const auto *source_type = &provider.type();
    if (inherited == parent_->inherited_records_.end()) {
        auto context = WiringContext{*parent_};
        provider = details::materialize_boundary_reference(provider);
    } else {
        source_type = parent_->inherited_requirements_[inherited->requirement].type;
    }

    const auto existing = std::ranges::find_if(context_bindings_, [&](const auto &requirement) {
        return requirement.name == name && requirement.type == source_type &&
               requirement.provider.record() == provider.record();
    });
    if (existing != context_bindings_.end()) {
        return existing->point;
    }

    auto point = add_inherited_requirement(details::NestedBindingIdentity::named(name), name, *source_type, provider);
    context_bindings_.push_back({name, source_type, provider, point});
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

bool GraphWiring::input_consumes(WiredInput input, WiringPoint point) const noexcept {
    if (!point.has_output() || point.record()->graph_ != this) {
        return false;
    }

    if (input.is_inherited()) {
        return input.index() < inherited_records_.size() && inherited_records_[input.index()].record == point.record();
    }

    if (input.index() >= output_records_.size()) {
        return false;
    }

    auto *input_record = output_records_[input.index()];
    while (input_record != nullptr) {
        if (input_record == point.record()) {
            return true;
        }

        const auto materialization = std::ranges::find_if(
            materialized_wiring_points_, [&](const auto &entry) { return entry.result == input_record; });
        if (materialization == materialized_wiring_points_.end()) {
            return false;
        }
        input_record = materialization->record;
    }

    return false;
}

void GraphWiring::cache_wired_node(std::size_t hash, std::size_t node_index) {
    wired_nodes_.insert(util::linear_hash_table::hash_code(hash), static_cast<std::uint32_t>(node_index),
                        fixed_memory_);
}

std::size_t GraphWiring::register_node(WiredNode node) {
    const auto index = nodes_.size();
    nodes_.reserve(index + 1);
    node_output_records_.reserve(index + 1);

    nodes_.push_back(std::move(node));
    node_output_records_.push_back(nullptr);
    return index;
}

void GraphWiring::register_node_output(std::size_t node_index, WiringPoint output, std::optional<std::size_t> hash) {
    node_output_records_[node_index] = output.record();
    if (hash) {
        cache_wired_node(*hash, node_index);
    }
}

WiringPoint GraphWiring::register_record(std::unique_ptr<details::WiringRecord> record) {
    auto &result = *record;
    result.graph_ = this;
    wiring_records_.push_back(std::move(record));
    return details::WiringRecord::point(result);
}

WiringPoint GraphWiring::register_item_record(WiringPoint parent, item_id_t item,
                                              std::unique_ptr<details::WiringRecord> record) {
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
    wiring_records_.reserve(wiring_records_.size() + 1);
    outputs_.reserve(outputs_.size() + 1);
    output_records_.reserve(output_records_.size() + 1);

    auto &result = *record;
    const auto output_index = outputs_.size();
    if (output.parent != WiredOutput::npos) {
        output_items_.insert(output_item_hash(output.parent, output.item), static_cast<std::uint32_t>(output_index),
                             fixed_memory_);
    }

    result.graph_ = this;
    result.output_ = output_index;

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

    const auto output = point.record()->output_;
    if (!point.record()->has_wired_output()) {
        throw WiringError{"a WiringPoint has no wired output until it is consumed"};
    }

    return output;
}

} // namespace bg

namespace bg {

WiringPoint GraphWiring::require_inherited_time_series(details::NestedBindingIdentity identity, util::symbol name,
                                                       const TimeSeriesType &type) {
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

void GraphWiring::register_service_binding_consumer(const details::NestedBindingIdentity &identity,
                                                    std::size_t node_index) {
    if (node_index >= nodes_.size()) {
        throw WiringError{"a service binding consumer is not a node in this GraphWiring"};
    }

    const auto requirement =
        std::ranges::find(service_binding_requirements_, identity, &ServiceBindingRequirement::identity);
    if (requirement == service_binding_requirements_.end()) {
        throw WiringError{"the service binding requirement was not found"};
    }
    if (!std::ranges::contains(requirement->consumers, node_index)) {
        requirement->consumers.push_back(node_index);
    }
}

void GraphWiring::register_nested_service_consumers(const GraphWiring &nested, std::size_t node_index) {
    if (nested.parent_ != this) {
        throw WiringError{"nested service consumers must belong to a child GraphWiring"};
    }
    if (node_index >= nodes_.size()) {
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
        if (!std::ranges::contains(requirement.consumers, node_index)) {
            requirement.consumers.push_back(node_index);
        }
    }
}

std::size_t GraphWiring::output_node_index(WiringPoint point) const {
    return outputs_[output_index(point)].node_index;
}

} // namespace bg

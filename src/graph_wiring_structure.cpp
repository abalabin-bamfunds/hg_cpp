#include <bg/graph/details/graph_wiring_structure.hpp>

#include <bg/graph/graph_wiring.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/type/scalar_type.hpp>

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

constexpr auto unmapped = std::numeric_limits<std::size_t>::max();

void combine(std::size_t &value, std::size_t item) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    value ^= item + constant + (value << 6) + (value >> 2);
}

template <typename T> void combine_pointer(std::size_t &value, const T *item) noexcept {
    combine(value, std::hash<const void *>{}(item));
}

struct OutputStep {
    item_id_t item;
    const TimeSeriesType *type;
    const TimeSeriesRuntimeType *runtime_type;

    bool operator==(const OutputStep &) const noexcept = default;
};

struct Endpoint {
    bool boundary{};
    std::size_t node;
    std::vector<OutputStep> path;
};

struct ParameterShape {
    const Type *type;
    InputActivity activity;
    InputValidity validity;
    bool dynamic_activity;
    bool accepts_absent_scalar;
    VariadicMode variadic_mode;

    bool operator==(const ParameterShape &) const noexcept = default;
};

struct StoredScalar {
    const ScalarRuntimeType *runtime_type;
    std::optional<ScalarValue<>> value{};
};

struct NodeShape {
    NodeTemplateIdentity identity;
    std::vector<ParameterShape> parameters{};
    const TimeSeriesType *output;
    const TimeSeriesRuntimeType *output_runtime_type{};
    std::vector<const ScalarType *> state_types{};
    std::vector<const TimeSeriesType *> dynamic_reference_input_types{};
    std::vector<StoredScalar> scalars{};
    std::vector<Endpoint> inputs{};
    std::vector<std::size_t> service_bindings{};
};

std::size_t endpoint_path_hash(const Endpoint &endpoint) noexcept {
    auto result = std::size_t{};
    for (const auto &step : endpoint.path) {
        combine(result, static_cast<std::size_t>(step.item));
        combine_pointer(result, step.type);
        combine_pointer(result, step.runtime_type);
    }
    return result;
}

bool same_scalar(const StoredScalar &left, const StoredScalar &right) noexcept {
    if (left.runtime_type != right.runtime_type || left.value.has_value() != right.value.has_value()) {
        return false;
    }
    return !left.value || left.value->view().equals(right.value->view());
}

bool same_node_label(const NodeShape &left, const NodeShape &right) {
    return left.identity == right.identity && left.parameters == right.parameters && left.output == right.output &&
           left.output_runtime_type == right.output_runtime_type && left.state_types == right.state_types &&
           left.dynamic_reference_input_types == right.dynamic_reference_input_types &&
           left.scalars.size() == right.scalars.size() &&
           std::ranges::equal(left.scalars, right.scalars, same_scalar) && left.inputs.size() == right.inputs.size() &&
           left.service_bindings == right.service_bindings;
}

std::size_t node_label_hash(const NodeShape &node) noexcept {
    auto result = node.identity.hash();
    for (const auto &parameter : node.parameters) {
        combine_pointer(result, parameter.type);
        combine(result, static_cast<std::size_t>(parameter.activity));
        combine(result, static_cast<std::size_t>(parameter.validity));
        combine(result, parameter.dynamic_activity);
        combine(result, parameter.accepts_absent_scalar);
        combine(result, static_cast<std::size_t>(parameter.variadic_mode));
    }
    combine_pointer(result, node.output);
    combine_pointer(result, node.output_runtime_type);
    for (const auto *type : node.state_types) {
        combine_pointer(result, type);
    }
    for (const auto *type : node.dynamic_reference_input_types) {
        combine_pointer(result, type);
    }
    for (const auto &scalar : node.scalars) {
        combine_pointer(result, scalar.runtime_type);
        combine(result, scalar.value ? scalar.value->hash() : 0);
    }
    for (const auto binding : node.service_bindings) {
        combine(result, binding);
    }
    return result;
}

Endpoint endpoint(const GraphWiring &wiring, std::size_t output_index) {
    auto result = Endpoint{};
    while (true) {
        const auto &output = wiring.outputs()[output_index];
        result.path.push_back({output.item, output.type, output.runtime_type});
        if (output.parent == WiredOutput::npos) {
            result.node = output.node_index;
            break;
        }
        output_index = output.parent;
    }
    std::ranges::reverse(result.path);
    return result;
}

std::size_t root_output_for(const GraphWiring &wiring, std::size_t node) {
    const auto found = std::ranges::find_if(wiring.outputs(), [&](const auto &output) {
        return output.node_index == node && output.parent == WiredOutput::npos;
    });
    if (found == wiring.outputs().end()) {
        throw std::invalid_argument{"a graph boundary node has no output"};
    }
    return static_cast<std::size_t>(found - wiring.outputs().begin());
}

} // namespace

struct GraphWiringStructure::Data {
    std::vector<NodeShape> nodes;
    std::vector<const TimeSeriesType *> boundaries;
    std::vector<const TimeSeriesType *> inherited_time_series;
    std::optional<Endpoint> result;
    const TimeSeriesType *result_type{};
    bool cacheable{true};
    std::size_t hash{};
};

namespace {

std::size_t structure_hash(const GraphWiringStructure::Data &data) {
    auto structural_hashes = std::vector<std::size_t>(data.nodes.size());
    auto nodes = std::vector<std::size_t>{};
    nodes.reserve(data.nodes.size());
    for (auto index = std::size_t{}; index < data.nodes.size(); ++index) {
        auto hash = node_label_hash(data.nodes[index]);
        for (const auto &input : data.nodes[index].inputs) {
            combine(hash, input.boundary ? input.node : structural_hashes[input.node]);
            combine(hash, input.boundary);
            combine(hash, endpoint_path_hash(input));
        }
        structural_hashes[index] = hash;
        nodes.push_back(hash);
    }
    std::ranges::sort(nodes);

    auto result = std::size_t{};
    for (const auto node : nodes) {
        combine(result, node);
    }
    for (const auto *boundary : data.boundaries) {
        combine_pointer(result, boundary);
    }
    for (const auto *type : data.inherited_time_series) {
        combine_pointer(result, type);
    }
    combine(result, data.result.has_value());
    if (data.result) {
        combine(result, data.result->boundary ? data.result->node : structural_hashes[data.result->node]);
        combine(result, data.result->boundary);
        combine(result, endpoint_path_hash(*data.result));
    }
    combine_pointer(result, data.result_type);
    return result;
}

struct Mapping {
    std::vector<std::size_t> left;
    std::vector<std::size_t> right;
};

bool constrain(const GraphWiringStructure::Data &left, const GraphWiringStructure::Data &right, std::size_t left_node,
               std::size_t right_node, Mapping &mapping) {
    if (mapping.left[left_node] != unmapped) {
        return mapping.left[left_node] == right_node;
    }
    if (mapping.right[right_node] != unmapped || !same_node_label(left.nodes[left_node], right.nodes[right_node])) {
        return false;
    }

    mapping.left[left_node] = right_node;
    mapping.right[right_node] = left_node;
    const auto &left_inputs = left.nodes[left_node].inputs;
    const auto &right_inputs = right.nodes[right_node].inputs;
    for (auto index = std::size_t{}; index < left_inputs.size(); ++index) {
        const auto &left_input = left_inputs[index];
        const auto &right_input = right_inputs[index];
        if (left_input.boundary != right_input.boundary || left_input.path != right_input.path) {
            return false;
        }
        if (left_input.boundary) {
            if (left_input.node != right_input.node) {
                return false;
            }
            continue;
        }
        if (!constrain(left, right, left_input.node, right_input.node, mapping)) {
            return false;
        }
    }
    return true;
}

bool complete_mapping(const GraphWiringStructure::Data &left, const GraphWiringStructure::Data &right,
                      Mapping mapping) {
    const auto next = std::ranges::find(mapping.left, unmapped);
    if (next == mapping.left.end()) {
        return true;
    }
    const auto left_node = static_cast<std::size_t>(next - mapping.left.begin());

    for (auto right_node = std::size_t{}; right_node < right.nodes.size(); ++right_node) {
        if (mapping.right[right_node] != unmapped || !same_node_label(left.nodes[left_node], right.nodes[right_node])) {
            continue;
        }

        auto candidate = mapping;
        if (constrain(left, right, left_node, right_node, candidate) &&
            complete_mapping(left, right, std::move(candidate))) {
            return true;
        }
    }
    return false;
}

bool equivalent(const GraphWiringStructure::Data &left, const GraphWiringStructure::Data &right) {
    if (!left.cacheable || !right.cacheable || left.hash != right.hash || left.nodes.size() != right.nodes.size() ||
        left.boundaries.size() != right.boundaries.size() || left.boundaries != right.boundaries ||
        left.inherited_time_series != right.inherited_time_series ||
        left.result.has_value() != right.result.has_value() || left.result_type != right.result_type) {
        return false;
    }

    auto mapping = Mapping{std::vector<std::size_t>(left.nodes.size(), unmapped),
                           std::vector<std::size_t>(right.nodes.size(), unmapped)};
    if (left.result) {
        if (left.result->boundary != right.result->boundary || left.result->path != right.result->path) {
            return false;
        }
        if (left.result->boundary) {
            if (left.result->node != right.result->node) {
                return false;
            }
        } else if (!constrain(left, right, left.result->node, right.result->node, mapping)) {
            return false;
        }
    }
    return complete_mapping(left, right, std::move(mapping));
}

bool path_starts_with(std::span<const OutputStep> path, std::span<const OutputStep> prefix) {
    return path.size() >= prefix.size() && std::ranges::equal(path.first(prefix.size()), prefix);
}

std::shared_ptr<GraphWiringStructure::Data> snapshot(const GraphWiring &wiring,
                                                     std::span<const std::size_t> boundary_outputs,
                                                     std::optional<std::size_t> result_output) {
    auto data = std::make_shared<GraphWiringStructure::Data>();
    data->boundaries.reserve(boundary_outputs.size());
    data->inherited_time_series.reserve(wiring.inherited_requirements().size());
    for (const auto &requirement : wiring.inherited_requirements()) {
        data->inherited_time_series.push_back(requirement.type);
    }

    auto external_boundaries = std::vector<Endpoint>{};
    external_boundaries.reserve(boundary_outputs.size());
    auto excluded_nodes = std::vector<bool>(wiring.nodes().size());
    for (const auto output : boundary_outputs) {
        auto external = endpoint(wiring, output);
        if (std::ranges::any_of(external_boundaries, [&](const auto &existing) {
                return external.node == existing.node && (path_starts_with(external.path, existing.path) ||
                                                          path_starts_with(existing.path, external.path));
            })) {
            throw std::invalid_argument{"graph boundaries must identify distinct outputs"};
        }

        data->boundaries.push_back(external.path.back().type);
        excluded_nodes[external.node] = true;
        external_boundaries.push_back(std::move(external));
    }

    auto node_indices = std::vector<std::size_t>(wiring.nodes().size(), unmapped);
    data->nodes.reserve(wiring.nodes().size() - std::ranges::count(excluded_nodes, true));

    auto root_outputs = std::vector<const WiredOutput *>(wiring.nodes().size());
    for (const auto &output : wiring.outputs()) {
        if (output.parent == WiredOutput::npos) {
            root_outputs[output.node_index] = &output;
        }
    }

    for (auto original_index = std::size_t{}; original_index < wiring.nodes().size(); ++original_index) {
        if (excluded_nodes[original_index]) {
            continue;
        }

        const auto &wired = wiring.nodes()[original_index];
        auto node = NodeShape{
            .identity = wired.node_template().definition_identity(),
            .output = wired.signature().output,
        };
        data->cacheable &= node.identity.cacheable();
        node.parameters.reserve(wired.signature().parameters.size());
        for (const auto &parameter : wired.signature().parameters) {
            node.parameters.push_back({parameter.type, parameter.activity, parameter.validity,
                                       parameter.dynamic_activity, parameter.accepts_absent_scalar,
                                       parameter.variadic_mode});
        }
        node_indices[original_index] = data->nodes.size();
        if (root_outputs[original_index] != nullptr) {
            node.output_runtime_type = root_outputs[original_index]->runtime_type;
        }
        node.state_types.assign(wired.state_types().begin(), wired.state_types().end());
        node.dynamic_reference_input_types.assign(wired.dynamic_reference_input_types().begin(),
                                                  wired.dynamic_reference_input_types().end());
        for (const auto &scalar : wired.scalar_values()) {
            auto stored = StoredScalar{.runtime_type = &scalar.runtime_type()};
            const auto &type = scalar.runtime_type().type();
            data->cacheable &= type.supports_equality() && type.supports_hash();
            if (scalar.has_value()) {
                stored.value.emplace(Scalar<>::from(scalar.runtime_type(), scalar.data()));
            }
            node.scalars.push_back(std::move(stored));
        }
        for (auto binding_index = std::size_t{}; binding_index < wiring.service_binding_requirements().size();
             ++binding_index) {
            const auto &binding = wiring.service_binding_requirements()[binding_index];
            if (std::ranges::contains(binding.consumers, original_index)) {
                node.service_bindings.push_back(binding_index);
            }
        }
        data->nodes.push_back(std::move(node));
    }

    const auto externalize = [&](Endpoint value) {
        for (auto index = std::size_t{}; index < external_boundaries.size(); ++index) {
            const auto &boundary = external_boundaries[index];
            if (value.node != boundary.node || !path_starts_with(value.path, boundary.path)) {
                continue;
            }

            value.boundary = true;
            value.node = index;
            value.path.erase(value.path.begin(),
                             value.path.begin() + static_cast<std::ptrdiff_t>(boundary.path.size()));
            return value;
        }

        if (excluded_nodes[value.node]) {
            throw std::invalid_argument{"a graph uses an undeclared output from a boundary node"};
        }
        value.node = node_indices[value.node];
        return value;
    };

    for (auto original_index = std::size_t{}; original_index < wiring.nodes().size(); ++original_index) {
        if (excluded_nodes[original_index]) {
            continue;
        }

        auto &node = data->nodes[node_indices[original_index]];
        for (const auto input : wiring.nodes()[original_index].inputs()) {
            if (input.is_inherited()) {
                node.inputs.push_back({
                    .boundary = true,
                    .node = boundary_outputs.size() + input.index(),
                    .path = {},
                });
                continue;
            }
            node.inputs.push_back(externalize(endpoint(wiring, input.index())));
        }
    }

    if (result_output) {
        data->result = externalize(endpoint(wiring, *result_output));
        data->result_type = wiring.outputs()[*result_output].type;
    }
    return data;
}

} // namespace

bool GraphWiringStructure::cacheable() const noexcept {
    return data_ != nullptr && data_->cacheable;
}

std::size_t GraphWiringStructure::hash() const noexcept {
    return cacheable() ? data_->hash : 0;
}

std::size_t GraphWiringStructure::boundary_count() const noexcept {
    return data_ == nullptr ? 0 : data_->boundaries.size();
}

const TimeSeriesType &GraphWiringStructure::boundary_type(std::size_t index) const noexcept {
    return *data_->boundaries[index];
}

const TimeSeriesType *GraphWiringStructure::result_type() const noexcept {
    return data_ == nullptr ? nullptr : data_->result_type;
}

bool operator==(const GraphWiringStructure &left, const GraphWiringStructure &right) {
    if (left.data_ == right.data_) {
        return left.data_ != nullptr;
    }
    return left.data_ != nullptr && right.data_ != nullptr && equivalent(*left.data_, *right.data_);
}

GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring, std::span<const WiringPoint> boundaries,
                                            std::optional<WiringPoint> result) {
    auto boundary_outputs = std::vector<std::size_t>{};
    boundary_outputs.reserve(boundaries.size());
    for (const auto boundary : boundaries) {
        boundary_outputs.push_back(wiring.output_index(boundary));
    }
    auto data = snapshot(wiring, boundary_outputs, result ? std::optional{wiring.output_index(*result)} : std::nullopt);
    if (data->cacheable) {
        data->hash = structure_hash(*data);
    }
    return GraphWiringStructure{std::move(data)};
}

GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring, std::span<const std::size_t> boundary_nodes,
                                            std::optional<std::size_t> result_node) {
    auto boundary_outputs = std::vector<std::size_t>{};
    boundary_outputs.reserve(boundary_nodes.size());
    for (const auto boundary : boundary_nodes) {
        boundary_outputs.push_back(root_output_for(wiring, boundary));
    }
    auto data = snapshot(wiring, boundary_outputs,
                         result_node ? std::optional{root_output_for(wiring, *result_node)} : std::nullopt);
    if (data->cacheable) {
        data->hash = structure_hash(*data);
    }
    return GraphWiringStructure{std::move(data)};
}

} // namespace bg::details

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
    std::size_t node;
    std::vector<OutputStep> path;
};

struct ParameterShape {
    const Type *type;
    InputActivity activity;
    InputValidity validity;
    bool accepts_absent_scalar;
    VariadicMode variadic_mode;

    bool operator==(const ParameterShape &) const noexcept = default;
};

struct StoredScalar {
    const ScalarRuntimeType *runtime_type;
    std::optional<ScalarValue<>> value;
};

struct NodeShape {
    NodeTemplateIdentity identity;
    std::vector<ParameterShape> parameters;
    const TimeSeriesType *output;
    const TimeSeriesRuntimeType *output_runtime_type;
    std::vector<const ScalarType *> state_types;
    std::vector<const TimeSeriesType *> dynamic_reference_input_types;
    std::vector<StoredScalar> scalars;
    std::vector<Endpoint> inputs;
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
           std::ranges::equal(left.scalars, right.scalars, same_scalar) && left.inputs.size() == right.inputs.size();
}

std::size_t node_label_hash(const NodeShape &node) noexcept {
    auto result = node.identity.hash();
    for (const auto &parameter : node.parameters) {
        combine_pointer(result, parameter.type);
        combine(result, static_cast<std::size_t>(parameter.activity));
        combine(result, static_cast<std::size_t>(parameter.validity));
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
    std::vector<Endpoint> boundaries;
    std::optional<Endpoint> result;
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
            combine(hash, structural_hashes[input.node]);
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
    for (const auto &boundary : data.boundaries) {
        combine(result, structural_hashes[boundary.node]);
        combine(result, endpoint_path_hash(boundary));
    }
    combine(result, data.result.has_value());
    if (data.result) {
        combine(result, structural_hashes[data.result->node]);
        combine(result, endpoint_path_hash(*data.result));
    }
    return result;
}

struct Mapping {
    std::vector<std::size_t> left;
    std::vector<std::size_t> right;
};

bool constrain(const GraphWiringStructure::Data &left, const GraphWiringStructure::Data &right,
               std::size_t left_node, std::size_t right_node, Mapping &mapping) {
    if (mapping.left[left_node] != unmapped) {
        return mapping.left[left_node] == right_node;
    }
    if (mapping.right[right_node] != unmapped ||
        !same_node_label(left.nodes[left_node], right.nodes[right_node])) {
        return false;
    }

    mapping.left[left_node] = right_node;
    mapping.right[right_node] = left_node;
    const auto &left_inputs = left.nodes[left_node].inputs;
    const auto &right_inputs = right.nodes[right_node].inputs;
    for (auto index = std::size_t{}; index < left_inputs.size(); ++index) {
        if (left_inputs[index].path != right_inputs[index].path ||
            !constrain(left, right, left_inputs[index].node, right_inputs[index].node, mapping)) {
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
        if (mapping.right[right_node] != unmapped ||
            !same_node_label(left.nodes[left_node], right.nodes[right_node])) {
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
    if (!left.cacheable || !right.cacheable || left.hash != right.hash ||
        left.nodes.size() != right.nodes.size() || left.boundaries.size() != right.boundaries.size() ||
        left.result.has_value() != right.result.has_value()) {
        return false;
    }

    auto mapping = Mapping{std::vector<std::size_t>(left.nodes.size(), unmapped),
                           std::vector<std::size_t>(right.nodes.size(), unmapped)};
    for (auto index = std::size_t{}; index < left.boundaries.size(); ++index) {
        if (left.boundaries[index].path != right.boundaries[index].path ||
            !constrain(left, right, left.boundaries[index].node, right.boundaries[index].node, mapping)) {
            return false;
        }
    }
    if (left.result &&
        (left.result->path != right.result->path ||
         !constrain(left, right, left.result->node, right.result->node, mapping))) {
        return false;
    }
    return complete_mapping(left, right, std::move(mapping));
}

std::shared_ptr<GraphWiringStructure::Data> snapshot(const GraphWiring &wiring) {
    auto data = std::make_shared<GraphWiringStructure::Data>();
    data->nodes.reserve(wiring.nodes().size());

    auto root_outputs = std::vector<const WiredOutput *>(wiring.nodes().size());
    for (const auto &output : wiring.outputs()) {
        if (output.parent == WiredOutput::npos) {
            root_outputs[output.node_index] = &output;
        }
    }

    for (const auto &wired : wiring.nodes()) {
        auto node = NodeShape{
            .identity = wired.node_template().definition_identity(),
            .output = wired.signature().output,
        };
        data->cacheable &= node.identity.cacheable();
        node.parameters.reserve(wired.signature().parameters.size());
        for (const auto &parameter : wired.signature().parameters) {
            node.parameters.push_back({parameter.type, parameter.activity, parameter.validity,
                                       parameter.accepts_absent_scalar, parameter.variadic_mode});
        }
        const auto node_index = data->nodes.size();
        if (root_outputs[node_index] != nullptr) {
            node.output_runtime_type = root_outputs[node_index]->runtime_type;
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
        for (const auto input : wired.input_output_indices()) {
            node.inputs.push_back(endpoint(wiring, input));
        }
        data->nodes.push_back(std::move(node));
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

bool operator==(const GraphWiringStructure &left, const GraphWiringStructure &right) {
    if (left.data_ == right.data_) {
        return left.data_ != nullptr;
    }
    return left.data_ != nullptr && right.data_ != nullptr && equivalent(*left.data_, *right.data_);
}

GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring,
                                            std::span<const WiringPoint> boundaries,
                                            std::optional<WiringPoint> result) {
    auto data = snapshot(wiring);
    data->boundaries.reserve(boundaries.size());
    for (const auto boundary : boundaries) {
        data->boundaries.push_back(endpoint(wiring, wiring.output_index(boundary)));
    }
    if (result) {
        data->result = endpoint(wiring, wiring.output_index(*result));
    }
    if (data->cacheable) {
        data->hash = structure_hash(*data);
    }
    return GraphWiringStructure{std::move(data)};
}

GraphWiringStructure graph_wiring_structure(const GraphWiring &wiring,
                                            std::span<const std::size_t> boundary_nodes,
                                            std::optional<std::size_t> result_node) {
    auto data = snapshot(wiring);
    data->boundaries.reserve(boundary_nodes.size());
    for (const auto boundary : boundary_nodes) {
        data->boundaries.push_back(endpoint(wiring, root_output_for(wiring, boundary)));
    }
    if (result_node) {
        data->result = endpoint(wiring, root_output_for(wiring, *result_node));
    }
    if (data->cacheable) {
        data->hash = structure_hash(*data);
    }
    return GraphWiringStructure{std::move(data)};
}

} // namespace bg::details

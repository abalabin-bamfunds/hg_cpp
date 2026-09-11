#include <bg/graph/graph_wiring.hpp>

#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/scalar.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace bg {

namespace {

util::linear_hash_table::hash_type output_item_hash(std::size_t parent, item_id_t item) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    auto result = parent;
    result ^= static_cast<std::size_t>(item) + constant + (result << 6) + (result >> 2);
    return util::linear_hash_table::hash_code(result);
}

std::optional<Scalar<>> scalar_value(const details::wiring_argument &argument) {
    if (!argument.scalar_has_value()) {
        return std::nullopt;
    }
    if (const auto *view = argument.scalar_view()) {
        return Scalar<>::from(view->runtime_type(), view->data());
    }
    return Scalar<>::from(argument.scalar_runtime_type(), argument.scalar_data());
}

bool same_signature(const ResolvedSignature &left, const ResolvedSignature &right) noexcept {
    if (left.output != right.output || left.parameters.size() != right.parameters.size()) {
        return false;
    }

    return std::ranges::equal(left.parameters, right.parameters,
                              [](const auto &left_parameter, const auto &right_parameter) {
                                  return left_parameter.type == right_parameter.type;
                              });
}

bool same_scalar_arguments(std::span<const WiredScalar> stored, std::span<const details::wiring_argument> arguments) {
    auto scalar_index = std::size_t{};
    for (const auto &argument : arguments) {
        if (!argument.is_scalar()) {
            continue;
        }

        const auto &candidate = stored[scalar_index++];
        if (candidate.has_value() != argument.scalar_has_value()) {
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

const GraphWiring &WiringPoint::graph() const noexcept {
    return record_->graph();
}

const TimeSeriesType &WiringPoint::type() const noexcept {
    return record_->type();
}

GraphWiring::GraphWiring() : GraphWiring(*std::pmr::get_default_resource()) {}

GraphWiring::GraphWiring(std::pmr::memory_resource &memory)
    : memory_(&memory), fixed_memory_{&memory}, paths_{&fixed_memory_} {}

std::pmr::memory_resource &GraphWiring::variable_memory() noexcept {
    if (variable_memory_) {
        return *variable_memory_;
    }

    return *std::pmr::null_memory_resource();
}

std::pmr::memory_resource &GraphWiring::variable_memory(const ScalarRuntimeType &runtime_type) {
    if (runtime_type.uses_variable_memory() && !variable_memory_) {
        variable_memory_.emplace(memory_);
    }

    return variable_memory();
}

GraphWiring::~GraphWiring() {
    for (const auto &node : nodes_) {
        for (const auto &scalar : node.scalar_values()) {
            if (scalar.has_value() && scalar.runtime_type_->requires_destruction()) {
                scalar.runtime_type_->destroy(scalar.data_, variable_memory());
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

        scalar.runtime_type_->destroy(scalar.data_, variable_memory());
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
                                               std::span<details::wiring_argument> arguments,
                                               const TypeMap *seed_type_map) {
    for (const auto &argument : arguments) {
        if (argument.is_wiring_point()) {
            const auto source = argument.wiring_point();
            if (&source.graph() != this) {
                throw WiringError{"a WiringPoint belongs to another GraphWiring"};
            }
        }
    }

    return wiring_template.invoke(*this, arguments, seed_type_map);
}

std::optional<WiringPoint> GraphWiring::invoke_resolved(const WiringTemplate &wiring_template,
                                                        SignatureResolution resolution,
                                                        std::span<details::wiring_argument> arguments,
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
                                                        std::span<const std::size_t> input_output_indices,
                                                        std::span<const ScalarType *const> state_types,
                                                        std::span<const details::wiring_argument> arguments) {
    const auto found = wired_nodes_.find(util::linear_hash_table::hash_code(hash), [&](auto position) {
        const auto &node = nodes_[position];
        return &node.node_template() == &node_template && same_signature(node.signature(), signature) &&
               std::ranges::equal(node.input_output_indices(), input_output_indices) &&
               std::ranges::equal(node.state_types(), state_types) &&
               same_scalar_arguments(node.scalar_values(), arguments);
    });
    if (!found) {
        return std::nullopt;
    }

    return WiringPoint{*node_output_records_[*found]};
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
    node_output_records_[node_index] = output.record_;
    if (hash) {
        cache_wired_node(*hash, node_index);
    }
}

WiringPoint GraphWiring::register_record(std::unique_ptr<details::WiringRecord> record) {
    auto &result = *record;
    result.graph_ = this;
    wiring_records_.push_back(std::move(record));
    return WiringPoint{result};
}

WiringPoint GraphWiring::register_item_record(WiringPoint parent, item_id_t item,
                                              std::unique_ptr<details::WiringRecord> record) {
    if (&parent.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto existing = std::ranges::find_if(
        item_records_, [&](const auto &entry) { return entry.parent == parent.record_ && entry.item == item; });
    if (existing != item_records_.end()) {
        return WiringPoint{*existing->result};
    }

    item_records_.reserve(item_records_.size() + 1);
    auto result = register_record(std::move(record));
    item_records_.push_back({parent.record_, item, result.record_});
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
    return WiringPoint{result};
}

std::optional<WiringPoint> GraphWiring::find_output_item(std::size_t parent, item_id_t item) const {
    const auto found = output_items_.find(output_item_hash(parent, item), [&](auto position) {
        const auto &output = outputs_[position];
        return output.parent == parent && output.item == item;
    });
    if (!found) {
        return std::nullopt;
    }

    return WiringPoint{*output_records_[*found]};
}

std::size_t GraphWiring::output_index(WiringPoint point) const {
    if (&point.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto output = point.record_->output_;
    if (output == details::WiringRecord::no_output) {
        throw WiringError{"a WiringPoint has no wired output until it is consumed"};
    }

    return output;
}

} // namespace bg

namespace bg::details {

std::size_t wired_output_node_index(const GraphWiring &wiring, WiringPoint point) {
    return wiring.outputs_[wiring.output_index(point)].node_index;
}

} // namespace bg::details

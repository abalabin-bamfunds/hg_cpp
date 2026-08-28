#include <bg/graph/graph_wiring.hpp>

#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/type_map.hpp>

#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace bg {

GraphWiring::GraphWiring() : GraphWiring(*std::pmr::get_default_resource()) {}

GraphWiring::GraphWiring(std::pmr::memory_resource &memory)
    : memory_(&memory), fixed_memory_{&memory}, paths_{&fixed_memory_} {}

std::pmr::memory_resource &GraphWiring::variable_memory() noexcept {
    if (variable_memory_) {
        return *variable_memory_;
    }

    return *std::pmr::null_memory_resource();
}

std::pmr::memory_resource &GraphWiring::variable_memory(const ScalarStorageRepresentation &representation) {
    if (representation.uses_variable_memory() && !variable_memory_) {
        variable_memory_.emplace(memory_);
    }

    return variable_memory();
}

GraphWiring::~GraphWiring() {
    for (const auto &node : nodes_) {
        for (const auto &scalar : node.scalar_values()) {
            if (scalar.has_value() && scalar.storage_representation_->requires_destruction()) {
                scalar.storage_representation_->destroy(scalar.data_, variable_memory());
            }
        }
    }
}

void GraphWiring::destroy_scalars(std::span<const WiredScalar> scalars) noexcept {
    for (const auto &scalar : scalars) {
        if (!scalar.has_value()) {
            continue;
        }

        scalar.storage_representation_->destroy(scalar.data_, variable_memory());
        fixed_memory_.deallocate(scalar.data_, scalar.storage_representation_->size(),
                                 scalar.storage_representation_->alignment());
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
                                               std::span<details::wiring_argument> arguments) {
    std::vector<const Type *> argument_types;
    argument_types.reserve(arguments.size());

    for (const auto &argument : arguments) {
        if (argument.is_wiring_point()) {
            const auto source = argument.wiring_point();
            if (&source.graph() != this) {
                throw WiringError{"a WiringPoint belongs to another GraphWiring"};
            }

            const auto index = source.node_index();
            if (nodes_[index].signature().output == nullptr) {
                throw WiringError{"a WiringPoint has no output"};
            }

            argument_types.push_back(nodes_[index].signature().output);
        } else {
            argument_types.push_back(&argument.scalar_type());
        }
    }

    auto resolution = wiring_template.signature().resolve_with_bindings(argument_types);
    auto signature = std::move(resolution.signature);
    const auto *expected_output = signature.output;
    const auto label = wiring_template.label(signature, arguments, default_label());
    const auto &path = make_path(signature.name, label);
    auto context = WiringContext{*this, path};

    auto output = wiring_template.wire(*this, std::move(signature), resolution.bindings, arguments);

    if ((expected_output == nullptr) != !output.has_value()) {
        throw WiringError{"a wiring-template result does not match its output declaration"};
    }
    if (!output) {
        return std::nullopt;
    }
    if (&output->graph() != this) {
        throw WiringError{"a wiring-template returned a WiringPoint from another GraphWiring"};
    }

    const auto *actual_output = nodes_[output->node_index()].signature().output;
    auto bindings = TypeMap{};
    if (!expected_output->match(*actual_output, bindings)) {
        throw WiringError{"a wiring-template result does not satisfy its resolved output type"};
    }

    return output;
}

std::optional<WiringPoint> GraphWiring::add_node(const NodeTemplate &node_template, ResolvedSignature signature,
                                                 std::vector<const ScalarType *> state_types,
                                                 std::span<details::wiring_argument> arguments) {
    std::vector<std::size_t> source_indices;
    std::size_t source_count = 0;
    for (const auto &argument : arguments) {
        source_count += static_cast<std::size_t>(argument.is_wiring_point());
    }
    source_indices.reserve(source_count);

    for (const details::wiring_argument &argument : arguments) {
        if (argument.is_wiring_point()) {
            source_indices.push_back(argument.wiring_point().node_index());
        }
    }

    std::vector<WiredScalar> scalar_values;
    scalar_values.reserve(arguments.size() - source_count);

    try {
        for (auto i = std::size_t{}; i < arguments.size(); ++i) {
            const auto &argument = arguments[i];
            if (argument.is_wiring_point()) {
                continue;
            }

            const auto &representation = argument.scalar_storage_representation();
            if (!argument.scalar_has_value()) {
                if (!signature.parameters[i].accepts_absent_scalar) {
                    throw WiringError{"a required scalar parameter has no value"};
                }

                scalar_values.push_back(WiredScalar{representation, nullptr});
                continue;
            }

            auto *data = fixed_memory_.allocate(representation.size(), representation.alignment());

            try {
                if (const auto *view = argument.scalar_view()) {
                    view->copy_construct(data, representation, variable_memory(representation));
                } else {
                    representation.construct_from_value(data, argument.scalar_data(), variable_memory(representation));
                }
            } catch (...) {
                fixed_memory_.deallocate(data, representation.size(), representation.alignment());
                throw;
            }

            scalar_values.push_back(WiredScalar{representation, data});
        }
    } catch (...) {
        destroy_scalars(scalar_values);

        throw;
    }

    const auto has_output = signature.output != nullptr;
    const auto index = nodes_.size();
    auto node = WiredNode{node_template,
                          std::move(signature),
                          std::move(source_indices),
                          std::move(scalar_values),
                          std::move(state_types),
                          *WiringContext::current().path()};

    try {
        nodes_.push_back(std::move(node));
    } catch (...) {
        destroy_scalars(node.scalar_values());

        throw;
    }

    if (has_output) {
        return WiringPoint{*this, index};
    }

    return std::nullopt;
}

std::optional<WiringPoint> NodeTemplate::wire(GraphWiring &wiring, ResolvedSignature signature, const TypeMap &bindings,
                                              std::span<details::wiring_argument> arguments) const {
    return wiring.add_node(*this, std::move(signature), resolve_state_types(bindings), arguments);
}

} // namespace bg

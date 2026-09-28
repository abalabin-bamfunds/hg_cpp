#include <bg/node/node_template.hpp>

#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/scalar/scalar.hpp>
#include <bg/util/scope_exit.hpp>

#include <functional>

namespace bg {
namespace {

void combine_hash(std::size_t &result, std::size_t value) noexcept {
    constexpr auto constant = std::size_t{0x9e3779b9};
    result ^= value + constant + (result << 6) + (result >> 2);
}

template <typename T> void combine_pointer(std::size_t &result, const T *value) noexcept {
    combine_hash(result, std::hash<const void *>{}(value));
}

std::optional<Scalar<>> scalar_value(const WiringArgument &argument) {
    if (!argument.present()) {
        return std::nullopt;
    }
    return argument.scalar();
}

std::optional<std::size_t> node_hash(const NodeTemplate &node_template, const ResolvedSignature &signature,
                                     std::span<const WiredInput> inputs, std::span<const ScalarType *const> state_types,
                                     std::span<const WiringArgument> arguments) {
    auto result = std::size_t{};
    combine_pointer(result, &node_template);
    combine_pointer(result, signature.output);
    for (const auto &parameter : signature.parameters) {
        combine_pointer(result, parameter.type);
    }
    for (const auto input : inputs) {
        combine_hash(result, input.index());
        combine_hash(result, input.is_context());
    }
    for (const auto *state_type : state_types) {
        combine_pointer(result, state_type);
    }

    for (const auto &argument : arguments) {
        if (!argument.is_scalar()) {
            continue;
        }

        combine_pointer(result, &argument.type());
        if (!argument.present()) {
            combine_hash(result, 0);
            continue;
        }
        const auto &type = static_cast<const ScalarType &>(argument.type());
        if (!type.supports_equality() || !type.supports_hash()) {
            return std::nullopt;
        }

        combine_hash(result, scalar_value(argument)->hash());
    }

    return result;
}

} // namespace

std::optional<WiringPoint> NodeTemplate::wire(GraphWiring &wiring, ResolvedSignature signature, const TypeMap &type_map,
                                              std::span<const WiringArgument> arguments) const {
    if (std::ranges::any_of(arguments, &WiringArgument::is_graph)) {
        throw WiringError{"a graph argument cannot be stored by a node"};
    }

    auto materialized_arguments = std::vector<WiringArgument>{};
    materialized_arguments.reserve(arguments.size());
    for (const auto &argument : arguments) {
        materialized_arguments.push_back(details::wiring_argument_access::borrow(argument));
    }

    auto inputs = std::vector<WiredInput>{};
    for (auto index = std::size_t{}; index < materialized_arguments.size(); ++index) {
        const auto &argument = materialized_arguments[index];
        if (argument.is_time_series()) {
            const auto &parameter = static_cast<const TimeSeriesType &>(*signature.parameters[index].type);
            inputs.push_back(wiring.wire_input(argument.time_series(), parameter));
        }
    }

    auto state_types = resolve_state_types(type_map);
    auto dynamic_reference_inputs = dynamic_reference_input_types(type_map);
    const auto has_output = signature.output != nullptr;
    const auto hash = deduplicate_wiring() && has_output
                          ? node_hash(*this, signature, inputs, state_types, materialized_arguments)
                          : std::nullopt;
    if (hash) {
        if (const auto existing =
                wiring.find_wired_node(*hash, *this, signature, inputs, state_types, materialized_arguments)) {
            return existing;
        }
    }

    auto scalar_values = std::vector<WiredScalar>{};
    scalar_values.reserve(materialized_arguments.size() - inputs.size());
    auto release_scalars = util::scope_exit{[&]() noexcept { wiring.destroy_scalars(scalar_values); }};

    for (auto index = std::size_t{}; index < materialized_arguments.size(); ++index) {
        const auto &argument = materialized_arguments[index];
        if (argument.is_time_series()) {
            continue;
        }

        if (!argument.present()) {
            if (!signature.parameters[index].accepts_absent_scalar) {
                throw WiringError{"a required scalar parameter has no value"};
            }

            scalar_values.push_back(
                WiredScalar{details::wiring_argument_access::scalar_runtime_type(argument), nullptr});
            continue;
        }

        const auto scalar = argument.scalar();
        const auto &runtime_type = scalar.runtime_type();
        auto scalar_memory = wiring.variable_allocator(runtime_type);
        auto *data = wiring.fixed_memory_.allocate(runtime_type.size(), runtime_type.alignment());
        auto release_data = util::scope_exit{
            [&]() noexcept { wiring.fixed_memory_.deallocate(data, runtime_type.size(), runtime_type.alignment()); }};

        runtime_type.construct_from_input(data, scalar, scalar_memory);

        auto destroy_data = util::scope_exit{[&]() noexcept { runtime_type.destroy(data, scalar_memory); }};
        scalar_values.push_back(WiredScalar{runtime_type, data});
        destroy_data.release();
        release_data.release();
    }

    auto node = WiredNode{*this,
                          std::move(signature),
                          std::move(inputs),
                          std::move(scalar_values),
                          std::move(state_types),
                          std::move(dynamic_reference_inputs),
                          *WiringContext::current().path()};
    release_scalars.release();

    auto release_node_scalars = util::scope_exit{[&]() noexcept { wiring.destroy_scalars(node.scalar_values()); }};
    const auto node_index = wiring.register_node(std::move(node));
    release_node_scalars.release();

    if (!has_output) {
        return std::nullopt;
    }

    const auto output =
        details::add_wired_output(wiring, {node_index, WiredOutput::npos, 0, wiring.nodes_.back().signature().output});
    wiring.register_node_output(node_index, output, hash);
    return output;
}

} // namespace bg

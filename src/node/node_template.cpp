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

std::optional<Scalar<>> scalar_value(const details::wiring_argument &argument) {
    if (!argument.scalar_has_value()) {
        return std::nullopt;
    }
    if (const auto *view = argument.scalar_view()) {
        return Scalar<>::from(*view);
    }
    return Scalar<>::from(argument.scalar_runtime_type(), argument.scalar_data());
}

std::optional<std::size_t> node_hash(const NodeTemplate &node_template, const ResolvedSignature &signature,
                                     std::span<const std::size_t> input_output_indices,
                                     std::span<const ScalarType *const> state_types,
                                     std::span<const details::wiring_argument> arguments) {
    auto result = std::size_t{};
    combine_pointer(result, &node_template);
    combine_pointer(result, signature.output);
    for (const auto &parameter : signature.parameters) {
        combine_pointer(result, parameter.type);
    }
    for (const auto output : input_output_indices) {
        combine_hash(result, output);
    }
    for (const auto *state_type : state_types) {
        combine_pointer(result, state_type);
    }

    for (const auto &argument : arguments) {
        if (!argument.is_scalar()) {
            continue;
        }

        combine_pointer(result, &argument.scalar_type());
        if (!argument.scalar_has_value()) {
            combine_hash(result, 0);
            continue;
        }
        if (!argument.scalar_type().supports_equality() || !argument.scalar_type().supports_hash()) {
            return std::nullopt;
        }

        combine_hash(result, scalar_value(argument)->hash());
    }

    return result;
}

} // namespace

std::optional<WiringPoint> NodeTemplate::wire(GraphWiring &wiring, ResolvedSignature signature, const TypeMap &type_map,
                                              std::span<details::wiring_argument> arguments) const {
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        if (!arguments[index].is_wiring_point()) {
            continue;
        }

        const auto &parameter = static_cast<const TimeSeriesType &>(*signature.parameters[index].type);
        const auto source = details::materialize_wiring_point(arguments[index].wiring_point(), parameter);
        arguments[index].wiring_point(source);
    }

    auto input_output_indices = std::vector<std::size_t>{};
    for (const auto &argument : arguments) {
        if (argument.is_wiring_point()) {
            input_output_indices.push_back(wiring.output_index(argument.wiring_point()));
        }
    }

    auto state_types = resolve_state_types(type_map);
    const auto has_output = signature.output != nullptr;
    const auto hash = deduplicate_wiring() && has_output
                          ? node_hash(*this, signature, input_output_indices, state_types, arguments)
                          : std::nullopt;
    if (hash) {
        if (const auto existing =
                wiring.find_wired_node(*hash, *this, signature, input_output_indices, state_types, arguments)) {
            return existing;
        }
    }

    auto scalar_values = std::vector<WiredScalar>{};
    scalar_values.reserve(arguments.size() - input_output_indices.size());
    auto release_scalars = util::scope_exit{[&]() noexcept { wiring.destroy_scalars(scalar_values); }};

    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (argument.is_wiring_point()) {
            continue;
        }

        const auto &runtime_type = argument.scalar_runtime_type();
        if (!argument.scalar_has_value()) {
            if (!signature.parameters[index].accepts_absent_scalar) {
                throw WiringError{"a required scalar parameter has no value"};
            }

            scalar_values.push_back(WiredScalar{runtime_type, nullptr});
            continue;
        }

        auto &scalar_memory = wiring.variable_memory(runtime_type);
        auto *data = wiring.fixed_memory_.allocate(runtime_type.size(), runtime_type.alignment());
        auto release_data = util::scope_exit{[&]() noexcept {
            wiring.fixed_memory_.deallocate(data, runtime_type.size(), runtime_type.alignment());
        }};

        if (const auto *view = argument.scalar_view()) {
            view->copy_construct(data, runtime_type, scalar_memory);
        } else {
            runtime_type.construct_from_value(data, argument.scalar_data(), scalar_memory);
        }

        auto destroy_data = util::scope_exit{[&]() noexcept { runtime_type.destroy(data, scalar_memory); }};
        scalar_values.push_back(WiredScalar{runtime_type, data});
        destroy_data.release();
        release_data.release();
    }

    auto node = WiredNode{*this,
                          std::move(signature),
                          std::move(input_output_indices),
                          std::move(scalar_values),
                          std::move(state_types),
                          *WiringContext::current().path()};
    release_scalars.release();

    auto release_node_scalars = util::scope_exit{[&]() noexcept { wiring.destroy_scalars(node.scalar_values()); }};
    const auto node_index = wiring.register_node(std::move(node));
    release_node_scalars.release();

    if (!has_output) {
        return std::nullopt;
    }

    const auto output = details::add_wired_output(
        wiring, {node_index, WiredOutput::npos, 0, wiring.nodes_.back().signature().output});
    wiring.register_node_output(node_index, output, hash);
    return output;
}

} // namespace bg

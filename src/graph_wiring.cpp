#include <bg/graph/graph_wiring.hpp>

#include <bg/graph/wiring_error.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/node/node_template.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/util/scope_exit.hpp>

#include "time_series/ref/ref_node.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <string_view>

namespace bg {

namespace {

bool requires_reference_representation(const TimeSeriesType &parameter,
                                                     const TimeSeriesType &argument) noexcept {
    const auto *parameter_dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&parameter);
    const auto *argument_dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&argument);
    if (parameter_dictionary == nullptr || argument_dictionary == nullptr) {
        return false;
    }

    const auto &parameter_item = parameter_dictionary->value_type();
    const auto &argument_item = argument_dictionary->value_type();
    const auto parameter_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(&parameter_item) != nullptr;
    const auto argument_is_reference = dynamic_cast<const TimeSeriesReferenceType *>(&argument_item) != nullptr;
    if (parameter_is_reference != argument_is_reference) {
        return true;
    }

    return requires_reference_representation(parameter_item, argument_item);
}

} // namespace

std::size_t WiringPoint::node_index() const noexcept {
    return graph_->wiring_points()[record_index_].node_index;
}

const TimeSeriesType &WiringPoint::type() const noexcept {
    return *graph_->wiring_points()[record_index_].type;
}

WiringPoint WiringPoint::item(std::size_t index) const {
    return graph_->select_item(*this, index);
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
                                               const TypeMap *seed_bindings) {
    std::vector<const Type *> argument_types;
    argument_types.reserve(arguments.size());

    for (const auto &argument : arguments) {
        if (argument.is_wiring_point()) {
            const auto source = argument.wiring_point();
            if (&source.graph() != this) {
                throw WiringError{"a WiringPoint belongs to another GraphWiring"};
            }
            argument_types.push_back(&source.type());
        } else if (argument.is_free_time_series_list()) {
            argument_types.push_back(argument.free_time_series_list().type);
        } else if (argument.is_free_time_series_bundle()) {
            argument_types.push_back(argument.free_time_series_bundle().type);
        } else {
            argument_types.push_back(&argument.scalar_type());
        }
    }

    auto resolution = seed_bindings == nullptr
                          ? wiring_template.signature().resolve_with_bindings(argument_types)
                          : wiring_template.signature().resolve_with_bindings(argument_types, *seed_bindings);
    auto signature = std::move(resolution.signature);

    for (auto i = std::size_t{}; i < arguments.size(); ++i) {
        if (!arguments[i].is_free_time_series_list()) {
            continue;
        }

        const auto &argument = arguments[i].free_time_series_list();
        const auto *parameter = static_cast<const TimeSeriesType *>(signature.parameters[i].type);
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(parameter)) {
            parameter = &reference->target_type();
        }
        const auto *list = dynamic_cast<const TimeSeriesListType *>(parameter);
        if (list == nullptr || list->item_count() != argument.items.size()) {
            throw WiringError{"a free time series list requires a matching fixed TSL parameter"};
        }

        arguments[i].wiring_point(argument.materialize(argument.items, *list));
    }

    for (auto i = std::size_t{}; i < arguments.size(); ++i) {
        if (!arguments[i].is_free_time_series_bundle()) {
            continue;
        }

        const auto &argument = arguments[i].free_time_series_bundle();
        const auto *parameter = static_cast<const TimeSeriesType *>(signature.parameters[i].type);
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(parameter)) {
            parameter = &reference->target_type();
        }
        const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(parameter);
        if (bundle == nullptr) {
            throw WiringError{"a free time series bundle requires a TSB parameter"};
        }

        arguments[i].wiring_point(argument.materialize(argument.fields, *bundle));
    }

    for (auto i = std::size_t{}; i < arguments.size(); ++i) {
        if (!arguments[i].is_wiring_point()) {
            continue;
        }

        const auto &parameter = static_cast<const TimeSeriesType &>(*signature.parameters[i].type);
        const auto &argument = arguments[i].wiring_point().type();
        const auto *parameter_reference = dynamic_cast<const TimeSeriesReferenceType *>(&parameter);
        const auto *argument_reference = dynamic_cast<const TimeSeriesReferenceType *>(&argument);

        if (parameter_reference != nullptr) {
            if (argument_reference == nullptr) {
                arguments[i].wiring_point(make_reference(arguments[i].wiring_point(), *parameter_reference));
            }
            continue;
        }

        if (argument_reference == nullptr && requires_reference_representation(parameter, argument)) {
            const auto &reference = time_series_reference_type(parameter, TimeSeriesGuarantee::const_);
            arguments[i].wiring_point(make_reference(arguments[i].wiring_point(), reference));
        }
    }

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

    auto output_bindings = TypeMap{};
    if (!expected_output->match(output->type(), output_bindings)) {
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
            source_indices.push_back(argument.wiring_point().record_index());
        }
    }

    std::vector<WiredScalar> scalar_values;
    scalar_values.reserve(arguments.size() - source_count);
    auto release_scalars = util::scope_exit{[&]() noexcept { destroy_scalars(scalar_values); }};

    for (auto i = std::size_t{}; i < arguments.size(); ++i) {
        const auto &argument = arguments[i];
        if (argument.is_wiring_point()) {
            continue;
        }

        const auto &runtime_type = argument.scalar_runtime_type();
        if (!argument.scalar_has_value()) {
            if (!signature.parameters[i].accepts_absent_scalar) {
                throw WiringError{"a required scalar parameter has no value"};
            }

            scalar_values.push_back(WiredScalar{runtime_type, nullptr});
            continue;
        }

        auto &scalar_memory = variable_memory(runtime_type);
        auto *data = fixed_memory_.allocate(runtime_type.size(), runtime_type.alignment());
        auto release_data = util::scope_exit{
            [&]() noexcept { fixed_memory_.deallocate(data, runtime_type.size(), runtime_type.alignment()); }};

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

    const auto has_output = signature.output != nullptr;
    const auto index = nodes_.size();
    auto node = WiredNode{node_template,
                          std::move(signature),
                          std::move(source_indices),
                          std::move(scalar_values),
                          std::move(state_types),
                          *WiringContext::current().path()};
    release_scalars.release();

    auto release_node_scalars = util::scope_exit{[&]() noexcept { destroy_scalars(node.scalar_values()); }};
    nodes_.push_back(std::move(node));
    release_node_scalars.release();

    if (has_output) {
        const auto record = wiring_points_.size();
        wiring_points_.push_back({index, WiringPointRecord::npos, 0, nodes_.back().signature().output});
        return WiringPoint{*this, record};
    }

    return std::nullopt;
}

WiringPoint GraphWiring::select_item(WiringPoint source, std::size_t index) {
    if (&source.graph() != this) {
        throw WiringError{"a WiringPoint belongs to another GraphWiring"};
    }

    const auto *collection = dynamic_cast<const FixedItemTimeSeriesType *>(&source.type());
    if (collection == nullptr) {
        throw WiringError{"a WiringPoint does not identify a time series with fixed items"};
    }

    const auto &item_type = collection->item_type(index);
    const auto item = collection->item_id(index);
    const auto &source_record = wiring_points_[source.record_index()];
    if (source_record.parent == WiringPointRecord::npos) {
        const auto &node = nodes_[source_record.node_index];
        if (const auto input = node.node_template().output_item_source(index)) {
            return WiringPoint{*this, node.input_sources()[*input]};
        }
    }

    for (auto record = std::size_t{}; record < wiring_points_.size(); ++record) {
        const auto &candidate = wiring_points_[record];
        if (candidate.parent == source.record_index() && candidate.item == item) {
            return WiringPoint{*this, record};
        }
    }

    const auto record = wiring_points_.size();
    wiring_points_.push_back({source.node_index(), source.record_index(), item, &item_type});
    return WiringPoint{*this, record};
}

WiringPoint GraphWiring::make_reference(WiringPoint source, const TimeSeriesType &reference_type) {
    const auto existing = std::ranges::find_if(references_, [&](const auto &entry) {
        return entry.source == source.record_index() && entry.type == &reference_type;
    });
    if (existing != references_.end()) {
        return WiringPoint{*this, existing->reference};
    }

    const auto &type = static_cast<const TimeSeriesReferenceType &>(reference_type);
    const auto result = details::reference_node_template(source.type(), type)(source);
    if (!result) {
        throw WiringError{"a reference-making node produced no output"};
    }

    references_.push_back({source.record_index(), &reference_type, result->record_index()});
    return *result;
}

std::optional<WiringPoint> NodeTemplate::wire(GraphWiring &wiring, ResolvedSignature signature, const TypeMap &bindings,
                                              std::span<details::wiring_argument> arguments) const {
    return wiring.add_node(*this, std::move(signature), resolve_state_types(bindings), arguments);
}

} // namespace bg

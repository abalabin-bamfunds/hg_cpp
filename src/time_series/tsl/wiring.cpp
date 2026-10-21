#include <bg/time_series/tsl/details/wiring.hpp>
#include <bg/time_series/tsl/tsl.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/operators.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsl/tsl_free_runtime_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg::details {
namespace {

class FreeFixedTimeSeriesListNodeTemplate final : public NodeTemplate {
  public:
    explicit FreeFixedTimeSeriesListNodeTemplate(const TimeSeriesListType &type)
        : signature_{make_signature(type)} {}

    const Signature &default_signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }
    NodeFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &arguments) const override {
        return &free_fixed_time_series_list_runtime_type(
            static_cast<const TimeSeriesListType &>(*arguments.resolved_output_type()));
    }
    bool uses_time_series_input_runtime_type(const ResolvedSignature &, std::size_t) const noexcept override {
        return true;
    }
    InputNotification input_notification(std::size_t input_index) const noexcept override {
        return {&input_changed, input_index};
    }

  private:
    static Signature make_signature(const TimeSeriesListType &type) {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(type.item_count());
        for (auto index = std::size_t{}; index < type.item_count(); ++index) {
            parameters.push_back({util::symbol{std::to_string(index)}, &type.item_type(), active, optional});
        }
        return {util::symbol{"tsl"}, std::move(parameters), type};
    }

    static free_fixed_time_series_list_storage &storage(OutputChangeContext &context,
                                                        std::size_t node_index) noexcept {
        return *static_cast<free_fixed_time_series_list_storage *>(
            context.runtime_graph().node(node_index).output_value());
    }

    static void input_changed(OutputChangeContext &context, std::size_t node_index,
                              std::size_t input_index, OutputChange) {
        auto &value = storage(context, node_index);
        if (value.change_time != context.evaluation_time()) {
            value.updated.clear();
            value.change_time = context.evaluation_time();
        }
        value.updated.set(static_cast<std::uint32_t>(input_index),
                          context.runtime_graph().allocation().memory_resource());
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        static_cast<free_fixed_time_series_list_storage *>(runtime_node.output_value())->node = &node;
        if (runtime_node.input_count() == 0) {
            runtime_node.output_last_updated().publish(context->evaluation_time());
        }
    }

    static bool evaluate(const CompiledNode &node, void *runtime_storage,
                         EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_fixed_time_series_list_storage *>(
            runtime_node.output_value());
        auto &state = runtime_node.output_last_updated();
        if (free_list_valid(value)) {
            state.publish(context->evaluation_time());
        } else if (state.valid()) {
            state.invalidate(context->evaluation_time());
        }
        return true;
    }

    Signature signature_;
};

const NodeTemplate &free_fixed_time_series_list_node_template(const TimeSeriesListType &type) {
    static auto mutex = std::mutex{};
    static auto templates = std::vector<std::unique_ptr<FreeFixedTimeSeriesListNodeTemplate>>{};

    const auto lock = std::lock_guard{mutex};
    const auto existing = std::ranges::find_if(
        templates, [&](const auto &node_template) { return node_template->signature().output() == &type; });
    if (existing != templates.end()) {
        return **existing;
    }

    auto &result = *templates.emplace_back(std::make_unique<FreeFixedTimeSeriesListNodeTemplate>(type));
    return result;
}

WiringPoint materialize_fixed_time_series_list(std::span<const WiringPoint> items,
                                               const TimeSeriesListType &type) {
    if (std::ranges::any_of(items, [](WiringPoint item) {
            const auto input = item.record()->input_binding(item.type());
            return input && input->is_borrowed();
        })) {
        throw WiringError{"a free TSL cannot retain borrowed input bindings"};
    }
    return *free_fixed_time_series_list_node_template(type)(items);
}

class FreeFixedTimeSeriesListWiringRecord final : public WiringRecord {
  public:
    FreeFixedTimeSeriesListWiringRecord(const TimeSeriesListType &type, std::span<const WiringPoint> items)
        : WiringRecord{type}, items_{items.begin(), items.end()} {}

    WiringPoint item(std::size_t index) const { return items_.at(index); }

  private:
    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override {
        const auto *list = type_cast<TimeSeriesListType>(&target_type.target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent || list->item_count() != items_.size()) {
            throw WiringError{"a free fixed TSL requires a matching fixed TSL parameter"};
        }

        auto preparation = MaterialisationPreparation{};
        preparation.dependencies.reserve(items_.size());
        for (auto index = std::size_t{}; index < items_.size(); ++index) {
            preparation.dependencies.push_back(
                {items_[index], &list->item_type(index), {}, {}, std::nullopt, list->item_id(index)});
        }
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto *list = type_cast<TimeSeriesListType>(&target_type.target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent) {
            throw WiringError{"a free fixed TSL requires a fixed TSL parameter"};
        }
        if (list->item_count() != items_.size()) {
            throw WiringError{"a free fixed TSL requires a matching fixed TSL parameter"};
        }

        const auto result = materialize_fixed_time_series_list(items_, *list);
        return wiring().materialize(result, target_type);
    }

    std::vector<WiringPoint> items_;
};

class TimeSeriesListItemWiringRecord final : public WiringRecord {
  public:
    TimeSeriesListItemWiringRecord(WiringPoint list, const TimeSeriesListType &type, std::size_t index)
        : WiringRecord{type.item_type(index)}, list_{list}, item_{type.item_id(index)} {}

  private:
    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override {
        const auto &type = type_cast<TimeSeriesListType>(list_.type().target_type());
        auto preparation = WiringRecord::prepare_materialisation(target_type);
        preparation.dependencies.push_back({list_, &type, {}, {}, std::nullopt, item_});
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto &type = type_cast<TimeSeriesListType>(list_.type().target_type());
        auto &graph_wiring = wiring();
        const auto list = graph_wiring.materialize(list_, type);
        const auto item = graph_wiring.output_item(list, item_, this->type());
        return graph_wiring.materialize(item, requested_type);
    }

    WiringPoint list_;
    item_id_t item_;
};

class TimeSeriesListVariadicWiringOperations final : public VariadicWiringOperations {
  public:
    void validate_parameter(const TimeSeriesType &type, VariadicMode mode) const override {
        const auto *list = type_cast<TimeSeriesListType>(&type.target_type());
        if (list == nullptr) {
            throw std::invalid_argument{"a variadic TSL parameter requires a TSL"};
        }
        if (list->size().value() == std::optional{std::dynamic_extent}) {
            throw std::invalid_argument{"a variadic TSL parameter requires a fixed size or size variable"};
        }
        if (mode != VariadicMode::positional) {
            throw std::invalid_argument{"a variadic TSL parameter requires var_args"};
        }
    }

    VariadicCapture capture(const TimeSeriesType &parameter_type, VariadicMode,
                            std::vector<WiringArgument> arguments, allocator *memory) const override {
        auto captured = std::vector<WiringArgument>{};
        auto item_types = std::vector<const TimeSeriesType *>{};
        captured.reserve(arguments.size());
        item_types.reserve(arguments.size());

        for (auto &argument : arguments) {
            if (argument.is_scalar()) {
                throw WiringError{"a variadic TSL accepts only time series arguments"};
            }

            const auto name = util::symbol{std::to_string(item_types.size())};
            argument.set_variadic_name(name, VariadicMode::positional);
            item_types.push_back(&argument.time_series().type());
            captured.push_back(memory == nullptr ? WiringArgument::borrow(argument) : argument.clone(*memory));
        }

        if (!item_types.empty()) {
            try {
                return {&TimeSeriesListType::from_items(item_types), std::move(captured)};
            } catch (const std::invalid_argument &error) {
                throw WiringError{error.what()};
            }
        }

        const auto &parameter = type_cast<TimeSeriesListType>(parameter_type.target_type());
        auto type_map = TypeMap{};
        const TimeSeriesType *item;
        try {
            const auto &resolved_item = parameter.item_type().resolve(type_map);
            item = type_cast<TimeSeriesType>(&resolved_item);
            if (item == nullptr) {
                throw WiringError{"an empty variadic TSL requires a concrete item type"};
            }
        } catch (const TypeResolutionError &) {
            throw WiringError{"an empty variadic TSL requires a concrete item type"};
        }

        try {
            return {&time_series_list_type(*item, 0, parameter.guarantee().resolve(type_map)),
                    std::move(captured)};
        } catch (const TypeResolutionError &) {
            throw WiringError{"an empty variadic TSL requires a concrete collection guarantee"};
        }
    }

    bool accepts_direct(const TimeSeriesType &type) const noexcept override {
        const auto *list = type_cast<TimeSeriesListType>(&type.target_type());
        return list != nullptr && list->item_count() != std::dynamic_extent;
    }

    std::size_t item_count(const TimeSeriesType &type) const noexcept override {
        return type_cast<TimeSeriesListType>(type.target_type()).item_count();
    }

    util::symbol item_name(const TimeSeriesType &, std::size_t index) const override {
        return util::symbol{std::to_string(index)};
    }

    std::vector<MaterialisationDependency>
    materialisation_dependencies(std::span<const WiringArgument> arguments,
                                 const TimeSeriesType &target_type) const override {
        const auto *list = type_cast<TimeSeriesListType>(&target_type.target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent) {
            throw WiringError{"variadic TSL wiring requires a fixed TSL"};
        }
        if (list->item_count() != arguments.size()) {
            throw WiringError{"a variadic TSL requires a matching fixed TSL parameter"};
        }

        auto dependencies = std::vector<MaterialisationDependency>{};
        dependencies.reserve(arguments.size());
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            if (arguments[index].is_time_series()) {
                dependencies.push_back({arguments[index].time_series(), &list->item_type(index)});
            }
        }
        return dependencies;
    }

    const TimeSeriesType &item_type(const TimeSeriesType &type, std::size_t) const override {
        return type_cast<TimeSeriesListType>(type.target_type()).item_type();
    }

    WiringPoint item(WiringPoint collection, std::size_t index) const override {
        return time_series_list_item(collection, index);
    }

    std::optional<std::span<const WiringArgument>>
    original_arguments(WiringPoint) const noexcept override {
        return std::nullopt;
    }

    WiringPoint materialize(std::span<const WiringArgument> arguments,
                            const TimeSeriesType &target_type) const override {
        auto items = std::vector<WiringPoint>{};
        items.reserve(arguments.size());
        for (const auto &argument : arguments) {
            items.push_back(argument.time_series());
        }

        const auto *list = type_cast<TimeSeriesListType>(&target_type.target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent) {
            throw WiringError{"variadic TSL wiring requires a fixed TSL"};
        }

        const auto result = materialize_fixed_time_series_list(items, *list);
        return result.record()->wiring().materialize(result, target_type);
    }
};

} // namespace

WiringPoint make_free_fixed_time_series_list(std::span<const WiringPoint> items) {
    auto item_types = std::vector<const TimeSeriesType *>{};
    item_types.reserve(items.size());
    for (const auto item : items) {
        item_types.push_back(&item.type());
    }

    const TimeSeriesListType *type;
    try {
        type = &TimeSeriesListType::from_items(item_types);
    } catch (const std::invalid_argument &error) {
        throw WiringError{error.what()};
    }

    return add_wiring_record(std::make_unique<FreeFixedTimeSeriesListWiringRecord>(*type, items));
}

WiringPoint make_empty_fixed_time_series_list(const TimeSeriesType &item_type) {
    if (!item_type.is_resolved()) {
        throw WiringError{"an empty fixed TSL requires a resolved item type"};
    }

    const auto &type = time_series_list_type(item_type, 0, TimeSeriesGuarantee::ordinary);
    return add_wiring_record(std::make_unique<FreeFixedTimeSeriesListWiringRecord>(type, std::span<const WiringPoint>{}));
}

WiringPoint time_series_list_item(WiringPoint list, std::size_t index) {
    const auto *semantic_type = type_cast<TimeSeriesListType>(&list.type().target_type());
    if (semantic_type == nullptr) {
        throw WiringError{"TSL item selection requires a TSL"};
    }
    if (index >= semantic_type->item_count()) {
        throw WiringError{"time series list index is out of range"};
    }

    if (list.record()->input_binding(*semantic_type)) {
        const auto &reference_type = time_series_reference_type(*semantic_type, TimeSeriesGuarantee::const_);
        const auto reference = list.record()->wiring().materialize(list, reference_type);
        return static_cast<WiringPoint>(operators::getitem(reference, static_cast<std::int64_t>(index)));
    }

    const auto &list_record = *list.record();
    if (const auto *free = dynamic_cast<const FreeFixedTimeSeriesListWiringRecord *>(&list_record)) {
        return free->item(index);
    }

    const auto item = semantic_type->item_id(index);
    auto selected = std::make_unique<TimeSeriesListItemWiringRecord>(list, *semantic_type, index);
    return add_item_wiring_record(list, item, std::move(selected));
}

const VariadicWiringOperations &time_series_list_variadic_wiring_operations() noexcept {
    static const auto operations = TimeSeriesListVariadicWiringOperations{};
    return operations;
}

} // namespace bg::details

namespace bg {

wp<> tsl(std::span<const WiringPoint> items) {
    return wp<>{details::make_free_fixed_time_series_list(items)};
}

wp<> tsl(const TimeSeriesType &item_type) {
    return wp<>{details::make_empty_fixed_time_series_list(item_type)};
}

} // namespace bg

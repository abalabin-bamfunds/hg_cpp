#include <bg/time_series/tsl/details/wiring.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsl/tsl_free_runtime_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>

#include <algorithm>
#include <charconv>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg::details {
namespace {

const TimeSeriesType &target_type_of(const TimeSeriesType &type) noexcept {
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
        return reference->target_type();
    }
    return type;
}

const TimeSeriesListType &list_type(const TimeSeriesType &type) {
    const auto *list = dynamic_cast<const TimeSeriesListType *>(&target_type_of(type));
    if (list == nullptr) {
        throw WiringError{"variadic TSL wiring requires a TSL"};
    }
    return *list;
}

const TimeSeriesListType &fixed_list_type(const TimeSeriesType &type) {
    const auto &list = list_type(type);
    if (list.item_count() == std::dynamic_extent) {
        throw WiringError{"variadic TSL wiring requires a fixed TSL"};
    }
    return list;
}

const TimeSeriesListType &argument_type(std::span<const WiringPoint> items) {
    const auto &first = target_type_of(items.front().type());
    auto guarantee = *first.guarantee().value();
    const auto &shape = first.with_guarantee(TimeSeriesGuarantee::ordinary);

    for (const auto item : items | std::views::drop(1)) {
        const auto &candidate = target_type_of(item.type());
        if (&candidate.with_guarantee(TimeSeriesGuarantee::ordinary) != &shape) {
            throw WiringError{"a variadic TSL requires compatible item types"};
        }
        guarantee = weakest(guarantee, *candidate.guarantee().value());
    }

    return time_series_list_type(first.with_guarantee(guarantee), items.size(),
                                 TimeSeriesGuarantee::ordinary);
}

class FreeFixedTimeSeriesListNodeTemplate final : public NodeTemplate {
  public:
    explicit FreeFixedTimeSeriesListNodeTemplate(const TimeSeriesListType &type)
        : signature_{make_signature(type)} {}

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }
    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }
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
            runtime_node.output_last_updated() = context->evaluation_time();
        }
    }

    static void evaluate(const CompiledNode &node, void *runtime_storage,
                         EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_fixed_time_series_list_storage *>(
            runtime_node.output_value());
        runtime_node.output_last_updated() = free_list_valid(value) ? context->evaluation_time() : MIN_DT;
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
    const auto &runtime_type = free_fixed_time_series_list_runtime_type(type);
    const auto result = *free_fixed_time_series_list_node_template(type)(items);
    set_wired_output_runtime_type(result, runtime_type);
    return result;
}

class FreeFixedTimeSeriesListWiringRecord final : public WiringRecord {
  public:
    explicit FreeFixedTimeSeriesListWiringRecord(std::span<const WiringPoint> items)
        : WiringRecord{argument_type(items)}, items_{items.begin(), items.end()} {}

    WiringPoint item(std::size_t index) const { return items_.at(index); }

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto &list = fixed_list_type(target_type);
        if (list.item_count() != items_.size()) {
            throw WiringError{"a free fixed TSL requires a matching fixed TSL parameter"};
        }

        const auto result = materialize_fixed_time_series_list(items_, list);
        return materialize_wiring_point(result, target_type);
    }

    std::vector<WiringPoint> items_;
};

class TimeSeriesListItemWiringRecord final : public WiringRecord {
  public:
    TimeSeriesListItemWiringRecord(WiringPoint list, std::size_t index)
        : WiringRecord{list_type(list.type()).item_type(index)}, list_{list},
          item_{list_type(list.type()).item_id(index)} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto &type = list_type(list_.type());
        const auto list = materialize_wiring_point(list_, type);
        const auto item = wiring_point_for_item(list, item_, this->type());
        return materialize_wiring_point(item, requested_type);
    }

    WiringPoint list_;
    item_id_t item_;
};

class TimeSeriesListVariadicWiringOperations final : public VariadicWiringOperations {
  public:
    void validate_parameter(const TimeSeriesType &type, VariadicMode mode) const override {
        const auto &list = list_type(type);
        if (list.size().value() == std::optional{std::dynamic_extent}) {
            throw std::invalid_argument{"a variadic TSL parameter requires a fixed size or size variable"};
        }
        if (mode != VariadicMode::positional) {
            throw std::invalid_argument{"a variadic TSL parameter requires var_args"};
        }
    }

    VariadicCapture capture(const TimeSeriesType &parameter_type, VariadicMode,
                            std::vector<WiringArgument> arguments, allocator *memory) const override {
        auto captured = std::vector<WiringArgument>{};
        auto items = std::vector<WiringPoint>{};
        captured.reserve(arguments.size());
        items.reserve(arguments.size());

        for (auto &argument : arguments) {
            if (argument.is_scalar()) {
                throw WiringError{"a variadic TSL accepts only time series arguments"};
            }
            if (argument.name().has_value() &&
                !wiring_argument_access::name_from_variadic_capture(argument)) {
                throw WiringError{"var_args does not accept named arguments"};
            }

            const auto name = util::symbol{std::to_string(items.size())};
            wiring_argument_access::set_variadic_name(argument, name);
            items.push_back(argument.time_series());
            captured.push_back(memory == nullptr ? wiring_argument_access::borrow(argument)
                                                 : wiring_argument_access::clone(argument, *memory));
        }

        if (!items.empty()) {
            return {&argument_type(items), std::move(captured)};
        }

        const auto &parameter = list_type(parameter_type);
        auto type_map = TypeMap{};
        const TimeSeriesType *item;
        try {
            const auto &resolved_item = parameter.item_type().resolve(type_map);
            item = dynamic_cast<const TimeSeriesType *>(&resolved_item);
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
        const auto &target = target_type_of(type);
        return dynamic_cast<const TimeSeriesListType *>(&target) != nullptr &&
               static_cast<const TimeSeriesListType &>(target).item_count() != std::dynamic_extent;
    }

    std::size_t item_count(const TimeSeriesType &type) const noexcept override {
        return static_cast<const TimeSeriesListType &>(target_type_of(type)).item_count();
    }

    util::symbol item_name(const TimeSeriesType &, std::size_t index) const override {
        return util::symbol{std::to_string(index)};
    }

    const TimeSeriesType &item_type(const TimeSeriesType &type, std::size_t) const override {
        return static_cast<const TimeSeriesListType &>(target_type_of(type)).item_type();
    }

    WiringPoint item(WiringPoint collection, std::size_t index) const override {
        return time_series_list_item(collection, index);
    }

    WiringPoint materialize(std::span<const WiringArgument> arguments,
                            const TimeSeriesType &target_type) const override {
        auto items = std::vector<WiringPoint>{};
        items.reserve(arguments.size());
        for (const auto &argument : arguments) {
            items.push_back(argument.time_series());
        }

        const auto result = materialize_fixed_time_series_list(items, fixed_list_type(target_type));
        return materialize_wiring_point(result, target_type);
    }
};

} // namespace

WiringPoint make_free_fixed_time_series_list(std::span<const WiringPoint> items) {
    return add_wiring_record(std::make_unique<FreeFixedTimeSeriesListWiringRecord>(items));
}

WiringPoint time_series_list_item(WiringPoint list, std::size_t index) {
    const auto &semantic_type = list_type(list.type());
    if (index >= semantic_type.item_count()) {
        throw WiringError{"time series list index is out of range"};
    }

    const auto &list_record = wiring_record(list);
    if (const auto *free = dynamic_cast<const FreeFixedTimeSeriesListWiringRecord *>(&list_record)) {
        return free->item(index);
    }

    const auto item = semantic_type.item_id(index);
    auto selected = std::make_unique<TimeSeriesListItemWiringRecord>(list, index);
    return add_item_wiring_record(list, item, std::move(selected));
}

const VariadicWiringOperations &time_series_list_variadic_wiring_operations() noexcept {
    static const auto operations = TimeSeriesListVariadicWiringOperations{};
    return operations;
}

} // namespace bg::details

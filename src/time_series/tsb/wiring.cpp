#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/variadic_wiring_record.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_arguments.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bg::details {
namespace {

class FreeTimeSeriesBundleNodeTemplate final : public NodeTemplate {
  public:
    explicit FreeTimeSeriesBundleNodeTemplate(const TimeSeriesBundleType &type) : signature_{make_signature(type)} {}

    const Signature &default_signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    NodeFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    const TimeSeriesRuntimeType *output_runtime_type(const WiringArguments &arguments) const override {
        return &free_time_series_bundle_runtime_type(
            static_cast<const TimeSeriesBundleType &>(*arguments.resolved_output_type()));
    }
    bool uses_time_series_input_runtime_type(const ResolvedSignature &, std::size_t) const noexcept override {
        return true;
    }
    InputNotification input_notification(std::size_t input_index) const noexcept override {
        return {&input_changed, input_index};
    }

  private:
    static Signature make_signature(const TimeSeriesBundleType &type) {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(type.item_count());
        for (auto index = std::size_t{}; index < type.item_count(); ++index) {
            parameters.push_back({type.item_name(index), &type.item_type(index), active, optional});
        }
        return {util::symbol{"bundle"}, std::move(parameters), type};
    }

    static free_time_series_bundle_storage &storage(OutputChangeContext &context, std::size_t node_index) noexcept {
        return *static_cast<free_time_series_bundle_storage *>(context.runtime_graph().node(node_index).output_value());
    }

    static void input_changed(OutputChangeContext &context, std::size_t node_index, std::size_t input_index,
                              OutputChange) {
        auto &value = storage(context, node_index);
        if (value.change_time != context.evaluation_time()) {
            value.modified.clear();
            value.change_time = context.evaluation_time();
        }
        value.modified.set(static_cast<std::uint32_t>(input_index), context.runtime_graph().allocation());
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        static_cast<free_time_series_bundle_storage *>(runtime_node.output_value())->node = &node;
        if (runtime_node.input_count() == 0) {
            runtime_node.output_last_updated().publish(context->evaluation_time());
        }
    }

    static bool evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_time_series_bundle_storage *>(runtime_node.output_value());
        auto &state = runtime_node.output_last_updated();
        if (free_bundle_valid(value)) {
            state.publish(context->evaluation_time());
        } else if (state.valid()) {
            state.invalidate(context->evaluation_time());
        }
        return true;
    }

    Signature signature_;
};

const NodeTemplate &free_time_series_bundle_node_template(const TimeSeriesBundleType &type) {
    static auto templates = std::vector<std::unique_ptr<FreeTimeSeriesBundleNodeTemplate>>{};

    const auto existing = std::ranges::find_if(
        templates, [&](const auto &node_template) { return node_template->signature().output() == &type; });
    if (existing != templates.end()) {
        return **existing;
    }

    auto &result = *templates.emplace_back(std::make_unique<FreeTimeSeriesBundleNodeTemplate>(type));
    return result;
}

const TimeSeriesType &time_series_bundle_field_type(const WiringArgument &argument) {
    if (argument.is_time_series()) {
        const auto *type = type_cast<TimeSeriesType>(&argument.type());
        if (type == nullptr) {
            throw WiringError{"a free TSB field requires a time series or scalar"};
        }
        return *type;
    }
    if (!argument.is_scalar()) {
        throw WiringError{"a free TSB field requires a time series or scalar"};
    }

    const auto *scalar = type_cast<ScalarType>(&argument.type());
    if (scalar == nullptr) {
        throw WiringError{"a free TSB scalar field requires a scalar type"};
    }

    const auto &type = time_series_value_type(*scalar, TimeSeriesGuarantee::const_);
    (void)resolve_time_series_runtime_type(type);
    return type;
}

class FreeTimeSeriesBundleWiringRecord final : public WiringRecord {
  public:
    FreeTimeSeriesBundleWiringRecord(const TimeSeriesBundleType &type, std::vector<WiringArgument> fields)
        : WiringRecord{type}, fields_{std::move(fields)} {}

    WiringArgument argument(std::size_t index) const { return WiringArgument::borrow(fields_.at(index)); }
    bool materialisation_ready_impl() const noexcept override {
        return std::ranges::all_of(fields_, [](const auto &field) {
            return !field.is_time_series() || field.time_series().record()->materialisation_ready();
        });
    }

  private:
    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"a free TSB requires a TSB parameter"};
        }

        auto preparation = MaterialisationPreparation{};
        preparation.dependencies.reserve(bundle->item_count());
        for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
            const auto name = bundle->item_name(index);
            const auto field = std::ranges::find(fields_, name, [](const auto &argument) { return *argument.name(); });
            if (field == fields_.end()) {
                throw WiringError{"a free TSB does not provide a required field"};
            }
            if (field->is_time_series()) {
                preparation.dependencies.push_back(
                    {field->time_series(), &bundle->item_type(index), {}, {}, name, bundle->item_id(index)});
            }
        }
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"a free TSB requires a TSB parameter"};
        }

        const auto result = materialize_time_series_bundle(fields_, *bundle);
        return wiring().materialize(result, target_type);
    }

    std::vector<WiringArgument> fields_;
};

class TimeSeriesBundleFieldWiringRecord final : public WiringRecord {
  public:
    TimeSeriesBundleFieldWiringRecord(WiringPoint bundle, const TimeSeriesBundleType &type,
                                      std::size_t index)
        : WiringRecord{type.item_type(index)}, bundle_{bundle}, field_{type.item_id(index)} {}

    bool materialisation_ready_impl() const noexcept override { return bundle_.record()->materialisation_ready(); }

  private:
    MaterialisationPreparation prepare_materialisation(const TimeSeriesType &target_type) const override {
        auto preparation = WiringRecord::prepare_materialisation(target_type);
        preparation.dependencies.push_back({bundle_, &bundle_.type(), {}, {}, std::nullopt, field_});
        return preparation;
    }

    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        auto &graph_wiring = wiring();
        const auto bundle = graph_wiring.materialize(bundle_, bundle_.type());
        const auto field = graph_wiring.output_item(bundle, field_, type());
        return graph_wiring.materialize(field, requested_type);
    }

    WiringPoint bundle_;
    item_id_t field_;
};

class TimeSeriesBundleVariadicWiringOperations final : public VariadicWiringOperations {
  public:
    void validate_parameter(const TimeSeriesType &type, VariadicMode) const override {
        if (type_cast<TimeSeriesBundleType>(&type.target_type()) == nullptr) {
            throw std::invalid_argument{"a variadic TSB parameter requires a TSB"};
        }
    }

    VariadicCapture capture(const TimeSeriesType &, VariadicMode mode,
                            std::vector<WiringArgument> arguments, allocator *memory) const override {
        auto fields = std::vector<SchemaField>{};
        auto captured = std::vector<WiringArgument>{};
        fields.reserve(arguments.size());
        captured.reserve(arguments.size());

        for (auto &argument : arguments) {
            if (argument.is_scalar() && !argument.present()) {
                continue;
            }

            if (mode == VariadicMode::named && !argument.name()) {
                throw WiringError{"a variadic TSB field requires a name"};
            }

            const auto name = mode == VariadicMode::positional
                                  ? util::symbol{std::to_string(fields.size())}
                                  : *argument.name();
            fields.push_back({name, &time_series_bundle_field_type(argument)});
            argument.set_variadic_name(name, mode);
            captured.push_back(memory == nullptr ? WiringArgument::borrow(argument) : argument.clone(*memory));
        }

        try {
            return {&TimeSeriesBundleType::from_fields(fields), std::move(captured)};
        } catch (const std::invalid_argument &error) {
            throw WiringError{error.what()};
        }
    }

    bool accepts_direct(const TimeSeriesType &type) const noexcept override {
        return type_cast<TimeSeriesBundleType>(&type.target_type()) != nullptr;
    }

    std::size_t item_count(const TimeSeriesType &type) const noexcept override {
        return type_cast<TimeSeriesBundleType>(type.target_type()).item_count();
    }

    util::symbol item_name(const TimeSeriesType &type, std::size_t index) const override {
        return type_cast<TimeSeriesBundleType>(type.target_type()).item_name(index);
    }

    const TimeSeriesType &item_type(const TimeSeriesType &type, std::size_t index) const override {
        return type_cast<TimeSeriesBundleType>(type.target_type()).item_type(index);
    }

    WiringPoint item(WiringPoint collection, std::size_t index) const override {
        return time_series_bundle_field(collection, index);
    }

    std::vector<MaterialisationDependency>
    materialisation_dependencies(std::span<const WiringArgument> arguments,
                                 const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"variadic TSB wiring requires a TSB"};
        }

        auto dependencies = std::vector<MaterialisationDependency>{};
        dependencies.reserve(bundle->item_count());
        for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
            const auto name = bundle->item_name(index);
            const auto argument = std::ranges::find_if(arguments, [&](const auto &candidate) {
                return candidate.name() == name;
            });
            if (argument == arguments.end()) {
                throw WiringError{"a variadic TSB does not provide a required field"};
            }
            if (argument->is_time_series()) {
                dependencies.push_back({argument->time_series(), &bundle->item_type(index), {}, {}, name,
                                        bundle->item_id(index)});
            }
        }
        return dependencies;
    }

    WiringPoint materialize(std::span<const WiringArgument> arguments,
                            const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"variadic TSB wiring requires a TSB"};
        }

        const auto result = materialize_time_series_bundle(arguments, *bundle);
        return result.record()->wiring().materialize(result, target_type);
    }
};

} // namespace

WiringPoint make_free_time_series_bundle(std::span<const WiringArgument> fields) {
    auto &wiring = WiringContext::current().wiring();
    auto field_types = std::vector<SchemaField>{};
    field_types.reserve(fields.size());
    auto owned_fields = std::vector<WiringArgument>{};
    owned_fields.reserve(fields.size());
    for (const auto &argument : fields) {
        if (argument.is_scalar() && !argument.present()) {
            continue;
        }
        if (!argument.name()) {
            throw WiringError{"a free TSB field requires a name"};
        }

        field_types.push_back({*argument.name(), &time_series_bundle_field_type(argument)});
        owned_fields.push_back(argument.clone(wiring.allocation()));
    }

    const TimeSeriesBundleType *type;
    try {
        type = &TimeSeriesBundleType::from_fields(field_types);
    } catch (const std::invalid_argument &error) {
        throw WiringError{error.what()};
    }

    return add_wiring_record(std::make_unique<FreeTimeSeriesBundleWiringRecord>(*type, std::move(owned_fields)));
}

WiringPoint materialize_time_series_bundle(std::span<const WiringArgument> fields,
                                           const TimeSeriesBundleType &type) {
    auto ordered = std::vector<WiringArgument>{};
    ordered.reserve(type.item_count());
    for (auto index = std::size_t{}; index < type.item_count(); ++index) {
        const auto name = type.item_name(index);
        const auto field = std::ranges::find(fields, name, [](const auto &argument) { return *argument.name(); });
        if (field == fields.end()) {
            throw WiringError{"a free TSB does not provide a required field"};
        }

        auto argument = WiringArgument::borrow(*field);
        argument.clear_name();
        argument.clear_variadic_capture_mode();
        if (argument.is_scalar()) {
            if (!argument.present()) {
                throw WiringError{"a free TSB does not provide a required field"};
            }
        } else if (!argument.is_time_series()) {
            throw WiringError{"a free TSB field requires a time series or scalar"};
        }

        if (argument.is_time_series()) {
            const auto point = argument.time_series();
            const auto input = point.record()->input_binding(point.type());
            if (input && input->is_borrowed()) {
                throw WiringError{"a free TSB cannot retain borrowed input bindings"};
            }
        }
        ordered.push_back(std::move(argument));
    }

    for (auto &argument : ordered) {
        if (argument.is_scalar()) {
            argument = WiringArgument{static_cast<WiringPoint>(operators::const_(argument))};
        }
    }

    return *free_time_series_bundle_node_template(type)(std::span<const WiringArgument>{ordered});
}

WiringPoint time_series_bundle_field(WiringPoint bundle, std::size_t index) {
    auto argument = time_series_bundle_argument(bundle, index);
    if (argument.is_time_series()) {
        return argument.time_series();
    }
    argument.clear_name();
    argument.clear_variadic_capture_mode();
    return operators::const_(argument);
}

WiringPoint time_series_bundle_field(WiringPoint bundle, util::symbol name) {
    const auto *type = type_cast<TimeSeriesBundleType>(&bundle.type().target_type());
    if (type == nullptr) {
        throw WiringError{"TSB field selection requires a TSB"};
    }

    for (auto index = std::size_t{}; index < type->item_count(); ++index) {
        if (type->item_name(index) == name) {
            return time_series_bundle_field(bundle, index);
        }
    }

    throw WiringError{"a TSB has no field with the requested name"};
}

WiringArgument time_series_bundle_argument(WiringPoint bundle, std::size_t index) {
    const auto *type = type_cast<TimeSeriesBundleType>(&bundle.type().target_type());
    if (type == nullptr) {
        throw WiringError{"TSB field selection requires a TSB"};
    }
    if (index >= type->item_count()) {
        throw WiringError{"time series bundle field index is out of range"};
    }

    const auto &record = *bundle.record();
    if (const auto *free = dynamic_cast<const FreeTimeSeriesBundleWiringRecord *>(&record)) {
        return free->argument(index);
    }
    if (const auto *variadic = variadic_wiring_record(bundle)) {
        return variadic->argument(index);
    }

    if (const auto input = record.input_binding(*type)) {
        const auto &reference_type = time_series_reference_type(*type, TimeSeriesGuarantee::const_);
        const auto reference = record.wiring().materialize(bundle, reference_type);
        return WiringArgument{static_cast<WiringPoint>(operators::getattr(reference, type->item_name(index)))};
    }

    const auto field = type->item_id(index);
    auto selected = std::make_unique<TimeSeriesBundleFieldWiringRecord>(bundle, *type, index);
    return WiringArgument{add_item_wiring_record(bundle, field, std::move(selected))};
}

const VariadicWiringOperations &time_series_bundle_variadic_wiring_operations() noexcept {
    static const auto operations = TimeSeriesBundleVariadicWiringOperations{};
    return operations;
}

} // namespace bg::details

namespace bg {

wp<> bundle(std::span<const WiringArgument> fields) {
    return wp<>{details::make_free_time_series_bundle(fields)};
}

} // namespace bg

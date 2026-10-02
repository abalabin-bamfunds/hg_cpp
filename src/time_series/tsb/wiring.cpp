#include <bg/time_series/tsb/details/wiring.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>

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

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    NodeFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
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
            runtime_node.output_last_updated() = context->evaluation_time();
        }
    }

    static bool evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_time_series_bundle_storage *>(runtime_node.output_value());
        runtime_node.output_last_updated() = free_bundle_valid(value) ? context->evaluation_time() : MIN_DT;
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

class FreeTimeSeriesBundleWiringRecord final : public WiringRecord {
  public:
    FreeTimeSeriesBundleWiringRecord(const TimeSeriesBundleType &type,
                                     std::span<const NamedBundleWiringPoint> fields)
        : WiringRecord{type}, fields_{fields.begin(), fields.end()} {}

    WiringPoint field(std::size_t index) const { return fields_.at(index).point; }

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"a free TSB requires a TSB parameter"};
        }

        const auto result = materialize_time_series_bundle(fields_, *bundle);
        return wiring().materialize(result, target_type);
    }

    std::vector<NamedBundleWiringPoint> fields_;
};

class TimeSeriesBundleFieldWiringRecord final : public WiringRecord {
  public:
    TimeSeriesBundleFieldWiringRecord(WiringPoint bundle, const TimeSeriesBundleType &type,
                                      std::size_t index)
        : WiringRecord{type.item_type(index)}, bundle_{bundle}, field_{type.item_id(index)} {}

  private:
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
        auto fields = std::vector<BundleField>{};
        auto captured = std::vector<WiringArgument>{};
        fields.reserve(arguments.size());
        captured.reserve(arguments.size());

        for (auto &argument : arguments) {
            if (argument.is_scalar() && !argument.present()) {
                continue;
            }

            const auto name = mode == VariadicMode::positional
                                  ? util::symbol{std::to_string(fields.size())}
                                  : *argument.name();
            const TimeSeriesType *type;
            if (argument.is_scalar()) {
                const auto *scalar = type_cast<ScalarType>(&argument.type());
                if (scalar == nullptr) {
                    throw WiringError{"a variadic TSB scalar argument requires a scalar type"};
                }

                const auto &value = time_series_value_type(*scalar, TimeSeriesGuarantee::const_);
                (void)resolve_time_series_runtime_type(value);
                type = &value;
            } else {
                type = type_cast<TimeSeriesType>(&argument.type());
                if (type == nullptr) {
                    throw WiringError{"a variadic TSB time series argument requires a time series type"};
                }
            }
            fields.push_back({name, type});
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

    WiringPoint materialize(std::span<const WiringArgument> arguments,
                            const TimeSeriesType &target_type) const override {
        const auto *bundle = type_cast<TimeSeriesBundleType>(&target_type.target_type());
        if (bundle == nullptr) {
            throw WiringError{"variadic TSB wiring requires a TSB"};
        }

        auto fields = std::vector<NamedBundleWiringPoint>{};
        fields.reserve(arguments.size());
        for (const auto &argument : arguments) {
            const auto point = argument.is_time_series()
                                   ? argument.time_series()
                                   : static_cast<WiringPoint>(operators::const_(argument));
            fields.push_back({*argument.name(), point});
        }
        const auto result = materialize_time_series_bundle(fields, *bundle);
        return wiring_record(result).wiring().materialize(result, target_type);
    }
};

} // namespace

WiringPoint make_free_time_series_bundle(std::span<const NamedBundleWiringPoint> fields) {
    auto field_types = std::vector<BundleField>{};
    field_types.reserve(fields.size());
    for (const auto &[name, point] : fields) {
        field_types.push_back({name, &point.type()});
    }

    const TimeSeriesBundleType *type;
    try {
        type = &TimeSeriesBundleType::from_fields(field_types);
    } catch (const std::invalid_argument &error) {
        throw WiringError{error.what()};
    }

    return add_wiring_record(std::make_unique<FreeTimeSeriesBundleWiringRecord>(*type, fields));
}

WiringPoint materialize_time_series_bundle(std::span<const NamedBundleWiringPoint> fields,
                                           const TimeSeriesBundleType &type) {
    auto ordered = std::vector<WiringPoint>{};
    ordered.reserve(type.item_count());
    for (auto index = std::size_t{}; index < type.item_count(); ++index) {
        const auto name = type.item_name(index);
        const auto field = std::ranges::find(fields, name, &NamedBundleWiringPoint::name);
        if (field == fields.end()) {
            throw std::invalid_argument{"a free TSB does not provide a required field"};
        }
        ordered.push_back(field->point);
    }

    const auto &runtime_type = free_time_series_bundle_runtime_type(type);
    const auto result = *free_time_series_bundle_node_template(type)(ordered);
    wiring_record(result).wiring().set_output_runtime_type(result, runtime_type);
    return result;
}

WiringPoint time_series_bundle_field(WiringPoint bundle, std::size_t index) {
    const auto *type = type_cast<TimeSeriesBundleType>(&bundle.type().target_type());
    if (type == nullptr) {
        throw WiringError{"TSB field selection requires a TSB"};
    }
    if (index >= type->item_count()) {
        throw WiringError{"time series bundle field index is out of range"};
    }

    const auto &record = wiring_record(bundle);
    if (const auto *free = dynamic_cast<const FreeTimeSeriesBundleWiringRecord *>(&record)) {
        return free->field(index);
    }

    const auto field = type->item_id(index);
    auto selected = std::make_unique<TimeSeriesBundleFieldWiringRecord>(bundle, *type, index);
    return add_item_wiring_record(bundle, field, std::move(selected));
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

const VariadicWiringOperations &time_series_bundle_variadic_wiring_operations() noexcept {
    static const auto operations = TimeSeriesBundleVariadicWiringOperations{};
    return operations;
}

} // namespace bg::details

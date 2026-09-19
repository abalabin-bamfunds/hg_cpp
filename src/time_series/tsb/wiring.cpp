#include <bg/time_series/tsb/details/wiring.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/variadic_wiring_operations.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/runtime_graph.hpp>
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

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }
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

    static void evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_time_series_bundle_storage *>(runtime_node.output_value());
        runtime_node.output_last_updated() = free_bundle_valid(value) ? context->evaluation_time() : MIN_DT;
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

const TimeSeriesType &target_type_of(const TimeSeriesType &type) noexcept {
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
        return reference->target_type();
    }
    return type;
}

const TimeSeriesBundleType &bundle_type(const TimeSeriesType &type) {
    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&target_type_of(type));
    if (bundle == nullptr) {
        throw WiringError{"variadic TSB wiring requires a TSB"};
    }
    return *bundle;
}

class FreeTimeSeriesBundleWiringRecord final : public WiringRecord {
  public:
    explicit FreeTimeSeriesBundleWiringRecord(std::span<const NamedBundleWiringPoint> fields)
        : WiringRecord{argument_type(fields)}, fields_{fields.begin(), fields.end()} {}

    WiringPoint field(std::size_t index) const { return fields_.at(index).point; }

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&target_type_of(target_type));
        if (bundle == nullptr) {
            throw WiringError{"a free TSB requires a TSB parameter"};
        }

        const auto result = materialize_time_series_bundle(fields_, *bundle);
        return materialize_wiring_point(result, target_type);
    }

    static const TimeSeriesBundleType &argument_type(std::span<const NamedBundleWiringPoint> fields) {
        for (auto left = std::size_t{}; left < fields.size(); ++left) {
            for (auto right = left + 1; right < fields.size(); ++right) {
                if (fields[left].name == fields[right].name) {
                    throw std::invalid_argument{"a free TSB has duplicate field names"};
                }
            }
        }

        auto schema_fields = std::vector<BundleField>{};
        schema_fields.reserve(fields.size());
        for (const auto &[name, point] : fields) {
            schema_fields.push_back({name, &point.type()});
        }
        return time_series_bundle_type(unnamed_bundle_schema(schema_fields), TimeSeriesGuarantee::ordinary);
    }

    std::vector<NamedBundleWiringPoint> fields_;
};

class TimeSeriesBundleFieldWiringRecord final : public WiringRecord {
  public:
    TimeSeriesBundleFieldWiringRecord(WiringPoint bundle, std::size_t index)
        : WiringRecord{bundle_type(bundle.type()).item_type(index)}, bundle_{bundle},
          field_{bundle_type(bundle.type()).item_id(index)} {}

  private:
    WiringPoint materialize(const TimeSeriesType &requested_type) const override {
        const auto bundle = materialize_wiring_point(bundle_, bundle_.type());
        const auto field = wiring_point_for_item(bundle, field_, type());
        return materialize_wiring_point(field, requested_type);
    }

    WiringPoint bundle_;
    item_id_t field_;
};

class TimeSeriesBundleVariadicWiringOperations final : public VariadicWiringOperations {
  public:
    void validate_parameter(const TimeSeriesType &type, VariadicMode) const override {
        (void)bundle_type(type);
    }

    VariadicCapture capture(const TimeSeriesType &, VariadicMode mode,
                            std::vector<WiringArgument> arguments, allocator *memory) const override {
        auto fields = std::vector<BundleField>{};
        auto names = std::vector<util::symbol>{};
        auto captured = std::vector<WiringArgument>{};
        fields.reserve(arguments.size());
        names.reserve(arguments.size());
        captured.reserve(arguments.size());

        for (auto &argument : arguments) {
            if (mode == VariadicMode::positional && argument.name().has_value() &&
                !wiring_argument_access::name_from_variadic_capture(argument)) {
                throw WiringError{"var_args does not accept named arguments"};
            }
            if (mode == VariadicMode::named && !argument.name().has_value()) {
                throw WiringError{"var_kwargs requires named arguments"};
            }
            if (mode == VariadicMode::named) {
                const auto name = *argument.name();
                if (std::ranges::find(names, name) != names.end()) {
                    throw WiringError{"a variadic parameter has duplicate argument names"};
                }
                names.push_back(name);
            }
            if (argument.is_scalar() && !argument.present()) {
                continue;
            }

            const auto name = mode == VariadicMode::positional
                                  ? util::symbol{std::to_string(fields.size())}
                                  : *argument.name();
            const auto *type = &argument.type();
            if (argument.is_scalar()) {
                const auto &value = time_series_value_type(
                    static_cast<const ScalarType &>(argument.type()), TimeSeriesGuarantee::const_);
                (void)resolve_time_series_runtime_type(value);
                type = &value;
            }
            fields.push_back({name, static_cast<const TimeSeriesType *>(type)});
            wiring_argument_access::set_variadic_name(argument, name);
            captured.push_back(memory == nullptr ? wiring_argument_access::borrow(argument)
                                                 : wiring_argument_access::clone(argument, *memory));
        }

        return {&time_series_bundle_type(unnamed_bundle_schema(fields), TimeSeriesGuarantee::ordinary),
                std::move(captured)};
    }

    bool accepts_direct(const TimeSeriesType &type) const noexcept override {
        return dynamic_cast<const TimeSeriesBundleType *>(&target_type_of(type)) != nullptr;
    }

    std::size_t item_count(const TimeSeriesType &type) const noexcept override {
        return static_cast<const TimeSeriesBundleType &>(target_type_of(type)).item_count();
    }

    util::symbol item_name(const TimeSeriesType &type, std::size_t index) const override {
        return static_cast<const TimeSeriesBundleType &>(target_type_of(type)).item_name(index);
    }

    const TimeSeriesType &item_type(const TimeSeriesType &type, std::size_t index) const override {
        return static_cast<const TimeSeriesBundleType &>(target_type_of(type)).item_type(index);
    }

    WiringPoint item(WiringPoint collection, std::size_t index) const override {
        return time_series_bundle_field(collection, index);
    }

    WiringPoint materialize(std::span<const WiringArgument> arguments,
                            const TimeSeriesType &target_type) const override {
        const auto &bundle = bundle_type(target_type);
        auto fields = std::vector<NamedBundleWiringPoint>{};
        fields.reserve(arguments.size());
        for (const auto &argument : arguments) {
            const auto point = argument.is_time_series()
                                   ? argument.time_series()
                                   : static_cast<WiringPoint>(operators::const_(argument));
            fields.push_back({*argument.name(), point});
        }
        return materialize_wiring_point(materialize_time_series_bundle(fields, bundle), target_type);
    }
};

} // namespace

WiringPoint make_free_time_series_bundle(std::span<const NamedBundleWiringPoint> fields) {
    return add_wiring_record(std::make_unique<FreeTimeSeriesBundleWiringRecord>(fields));
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
    set_wired_output_runtime_type(result, runtime_type);
    return result;
}

WiringPoint time_series_bundle_field(WiringPoint bundle, std::size_t index) {
    const auto &record = wiring_record(bundle);
    if (const auto *free = dynamic_cast<const FreeTimeSeriesBundleWiringRecord *>(&record)) {
        return free->field(index);
    }

    const auto &bundle_type = static_cast<const TimeSeriesBundleType &>(target_type_of(bundle.type()));
    const auto field = bundle_type.item_id(index);
    auto selected = std::make_unique<TimeSeriesBundleFieldWiringRecord>(bundle, index);
    return add_item_wiring_record(bundle, field, std::move(selected));
}

WiringPoint time_series_bundle_field(WiringPoint bundle, util::symbol name) {
    const auto &type = static_cast<const TimeSeriesBundleType &>(target_type_of(bundle.type()));
    for (auto index = std::size_t{}; index < type.item_count(); ++index) {
        if (type.item_name(index) == name) {
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

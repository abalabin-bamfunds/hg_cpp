#include <bg/time_series/tsb/details/wiring.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/wired_output.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
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

std::optional<WiringPoint> free_time_series_bundle_field(const WiringRecord &record, std::size_t index) {
    const auto *bundle = dynamic_cast<const FreeTimeSeriesBundleWiringRecord *>(&record);
    if (bundle == nullptr) {
        return std::nullopt;
    }
    return bundle->field(index);
}

} // namespace bg::details

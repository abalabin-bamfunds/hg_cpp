#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/named_argument.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace bg::details {

struct NamedBundleWiringPoint {
    util::symbol name;
    WiringPoint point;
};

class FreeTimeSeriesBundleNodeTemplate final : public NodeTemplate {
  public:
    explicit FreeTimeSeriesBundleNodeTemplate(const TimeSeriesBundleType &type) : signature_{make_signature(type)} {}

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }
    InputChangeCallback input_change_callback(std::size_t input_index) const noexcept override {
        return {&input_changed, input_index};
    }
  private:
    [[nodiscard]] static Signature make_signature(const TimeSeriesBundleType &type) {
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
        value.modified.set(static_cast<std::uint32_t>(input_index), context.runtime_graph().memory_resource());
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *) noexcept {
        static_cast<free_time_series_bundle_storage *>(Node{node, runtime_storage}.output_value())->node = &node;
    }

    static void evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const free_time_series_bundle_storage *>(runtime_node.output_value());
        runtime_node.output_last_updated() = free_bundle_valid(value) ? context->evaluation_time() : MIN_DT;
    }

    Signature signature_;
};

[[nodiscard]] inline WiringPoint materialize_time_series_bundle(std::span<const NamedBundleWiringPoint> fields,
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

    auto node = std::make_unique<FreeTimeSeriesBundleNodeTemplate>(type);
    const auto &runtime_type = free_time_series_bundle_runtime_type(type);
    const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node));
    const auto result = *owned(ordered);
    set_wired_output_runtime_type(result, runtime_type);
    return result;
}

class FreeTimeSeriesBundleWiringRecord final : public WiringRecord {
  public:
    template <std::size_t Size>
    explicit FreeTimeSeriesBundleWiringRecord(std::array<NamedBundleWiringPoint, Size> fields)
        : WiringRecord{argument_type(fields)}, fields_{fields.begin(), fields.end()} {}

    WiringPoint field(std::size_t index) const { return fields_.at(index).point; }

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto &value_type = target_type_of(target_type);
        const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&value_type);
        if (bundle == nullptr) {
            throw WiringError{"a free TSB requires a TSB parameter"};
        }

        const auto result = materialize_time_series_bundle(fields_, *bundle);
        return materialize_wiring_point(result, target_type);
    }

    static const TimeSeriesType &target_type_of(const TimeSeriesType &type) noexcept {
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
            return reference->target_type();
        }
        return type;
    }

    template <std::size_t Size>
    static const TimeSeriesBundleType &argument_type(const std::array<NamedBundleWiringPoint, Size> &fields) {
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

template <typename T> struct named_bundle_wiring_field : std::false_type {};

template <util::compile_time_string Name, typename Value>
struct named_bundle_wiring_field<named_argument<Name, Value>>
    : std::bool_constant<std::same_as<Value, WiringPoint> || is_typed_wiring_point<Value>> {};

template <typename T>
inline constexpr bool is_named_bundle_wiring_field = named_bundle_wiring_field<std::remove_cvref_t<T>>::value;

template <template <template <typename> typename> typename Schema> class BundleWiringFields {
    class Factory {
      public:
        explicit Factory(WiringPoint bundle) noexcept : bundle_{bundle} {}

        template <typename Field> wp<Field> make(std::size_t index) const {
            const auto &type = TSB<Schema>::type();
            return operators::getattr(bundle_, type.item_name(index)).template as<Field>();
        }

      private:
        WiringPoint bundle_;
    };

  public:
    explicit BundleWiringFields(WiringPoint bundle) : fields_{make(bundle)} {}

    const Schema<wp> *operator->() const noexcept { return &fields_; }

  private:
    static Schema<wp> make(WiringPoint bundle) {
        auto factory = Factory{bundle};
        return make_bundle_fields<Schema, wp>(factory);
    }

    Schema<wp> fields_;
};

} // namespace bg::details

namespace bg {

template <template <template <typename> typename> typename Schema, auto... Properties>
class wp<TSB<Schema, Properties...>> {
    using TimeSeries = TSB<Schema, Properties...>;

  public:
    wp(WiringPoint point) : point_{point} {
        if (!TimeSeries::type().accepts(point.type())) {
            throw WiringError{"a WiringPoint is incompatible with the requested type"};
        }
    }

    template <typename Source> explicit wp(wp<Source> source) : wp(static_cast<WiringPoint>(source)) {}

    operator WiringPoint() const noexcept { return point_; }

    const GraphWiring &graph() const noexcept { return point_.graph(); }
    const TimeSeriesType &type() const noexcept { return point_.type(); }

    template <typename Target> wp<Target> as() const { return point_.template as<Target>(); }

    auto operator->() const
        requires(!details::bundle_schema_variable_declaration<Schema<details::bundle_schema_field>>)
    {
        return details::BundleWiringFields<Schema>{point_};
    }

    using time_series_type = TimeSeries;

  private:
    WiringPoint point_;
};

template <typename... Fields>
    requires(sizeof...(Fields) != 0 && (details::is_named_bundle_wiring_field<Fields> && ...))
[[nodiscard]] wp<> bundle(Fields &&...fields) {
    auto points = std::array<details::NamedBundleWiringPoint, sizeof...(Fields)>{details::NamedBundleWiringPoint{
        util::symbol{std::remove_cvref_t<Fields>::name.view()}, static_cast<WiringPoint>(fields.value)}...};
    auto record = std::make_unique<details::FreeTimeSeriesBundleWiringRecord>(points);
    return wp<>{details::add_wiring_record(std::move(record))};
}

} // namespace bg

#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/deferred_wiring_point.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsb/tsb_free_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/util/named_argument.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
    explicit FreeTimeSeriesBundleNodeTemplate(const TimeSeriesBundleType &type)
        : runtime_type_{&free_time_series_bundle_runtime_type(type)}, signature_{make_signature(type)} {}

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    EvaluationFunction start_function(const ResolvedSignature &) const noexcept override { return &start; }
    const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                        std::span<const TimeSeriesRuntimeType *const>) const noexcept override {
        return runtime_type_;
    }
    bool uses_time_series_input_runtime_type(std::size_t) const noexcept override { return true; }
    InputChangeCallback input_change_callback(std::size_t input_index) const noexcept override {
        return {&input_changed, input_index};
    }
    std::optional<std::size_t> output_item_source(std::size_t item) const noexcept override {
        return item < signature_.parameter_count() ? std::optional{item} : std::nullopt;
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

    const TimeSeriesRuntimeType *runtime_type_;
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
            throw std::invalid_argument{"a free time series bundle does not provide a required field"};
        }
        ordered.push_back(field->point);
    }

    auto node = std::make_unique<FreeTimeSeriesBundleNodeTemplate>(type);
    const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node));
    return *owned(ordered);
}

template <std::size_t Size> class FreeTimeSeriesBundleWiringPoint final : public DeferredWiringPoint {
  public:
    explicit FreeTimeSeriesBundleWiringPoint(std::array<NamedBundleWiringPoint, Size> fields)
        : fields_{fields}, type_{&argument_type(fields)} {}

    const TimeSeriesType &type() const noexcept override { return *type_; }

    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&target_type);
        if (bundle == nullptr) {
            throw WiringError{"a free time series bundle requires a TSB parameter"};
        }

        return materialize_time_series_bundle(fields_, *bundle);
    }

    WiringPoint item(std::size_t index) const override { return fields_.at(index).point; }

  private:
    static const TimeSeriesBundleType &argument_type(const std::array<NamedBundleWiringPoint, Size> &fields) {
        for (auto left = std::size_t{}; left < fields.size(); ++left) {
            for (auto right = left + 1; right < fields.size(); ++right) {
                if (fields[left].name == fields[right].name) {
                    throw std::invalid_argument{"a free time series bundle has duplicate field names"};
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

    std::array<NamedBundleWiringPoint, Size> fields_;
    const TimeSeriesBundleType *type_;
};

template <typename T> struct named_bundle_wiring_field : std::false_type {};

template <util::compile_time_string Name, typename Value>
struct named_bundle_wiring_field<named_argument<Name, Value>>
    : std::bool_constant<std::same_as<Value, WiringPoint> || is_typed_wiring_point<Value>> {};

template <typename T>
inline constexpr bool is_named_bundle_wiring_field = named_bundle_wiring_field<std::remove_cvref_t<T>>::value;

template <util::compile_time_string Name, typename... Fields>
inline constexpr std::size_t named_bundle_wiring_field_index = [] {
    auto result = sizeof...(Fields);
    auto index = std::size_t{};
    ((std::remove_cvref_t<Fields>::name == Name ? result = index : result, ++index), ...);
    return result;
}();

} // namespace bg::details

namespace bg {

template <typename... Fields> class TimeSeriesBundleWiring {
  public:
    explicit TimeSeriesBundleWiring(WiringPoint point) : point_{point} {}

    operator WiringPoint() const noexcept { return point_; }

    template <util::compile_time_string Name> auto operator[](details::named_argument_name<Name>) const {
        constexpr auto Index = details::named_bundle_wiring_field_index<Name, Fields...>;
        static_assert(Index != sizeof...(Fields), "free time series bundle has no field with this name");
        using Field = std::tuple_element_t<Index, std::tuple<Fields...>>;
        using Value = decltype(Field::value);

        if constexpr (details::is_typed_wiring_point<Value>) {
            return Value{point_[Index]};
        } else {
            return point_[Index];
        }
    }

  private:
    WiringPoint point_;
};

template <typename... Fields>
    requires(sizeof...(Fields) != 0 && (details::is_named_bundle_wiring_field<Fields> && ...))
[[nodiscard]] auto bundle(Fields &&...fields) {
    auto points = std::array<details::NamedBundleWiringPoint, sizeof...(Fields)>{details::NamedBundleWiringPoint{
        util::symbol{std::remove_cvref_t<Fields>::name.view()}, static_cast<WiringPoint>(fields.value)}...};
    auto deferred = std::make_unique<details::FreeTimeSeriesBundleWiringPoint<sizeof...(Fields)>>(points);
    return TimeSeriesBundleWiring<std::remove_cvref_t<Fields>...>{details::defer_wiring_point(std::move(deferred))};
}

} // namespace bg

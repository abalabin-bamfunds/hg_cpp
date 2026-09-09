#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/graph/wiring_error.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsl/tsl_free_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {

template <std::size_t Size> class FreeFixedTimeSeriesListNodeTemplate final : public NodeTemplate {
    using Storage = free_fixed_time_series_list_storage<Size>;

  public:
    explicit FreeFixedTimeSeriesListNodeTemplate(const TimeSeriesListType &type)
        : runtime_type_{&free_fixed_time_series_list_runtime_type<Size>(type)}, signature_{make_signature(type)} {}

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
  private:
    [[nodiscard]] static Signature make_signature(const TimeSeriesListType &type) {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(Size);
        for (auto index = std::size_t{}; index < Size; ++index) {
            std::array<char, 24> buffer;
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), index);
            if (error != std::errc{}) {
                throw std::logic_error{"failed to create a TSL item name"};
            }
            const auto name = std::string_view{buffer.data(), static_cast<std::size_t>(end - buffer.data())};
            parameters.push_back({util::symbol{name}, &type.item_type(), active, optional});
        }
        return {util::symbol{"tsl"}, std::move(parameters), type};
    }

    static Storage &storage(OutputChangeContext &context, std::size_t node_index) noexcept {
        return *static_cast<Storage *>(context.runtime_graph().node(node_index).output_value());
    }

    static void input_changed(OutputChangeContext &context, std::size_t node_index, std::size_t input_index,
                              OutputChange) {
        auto &value = storage(context, node_index);
        value.changes.mark(input_index, context.evaluation_time());
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *) noexcept {
        static_cast<Storage *>(Node{node, runtime_storage}.output_value())->node = &node;
    }

    static void evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        const auto runtime_node = Node{node, runtime_storage};
        const auto &value = *static_cast<const Storage *>(runtime_node.output_value());
        runtime_node.output_last_updated() = free_list_valid(value) ? context->evaluation_time() : MIN_DT;
    }

    const TimeSeriesRuntimeType *runtime_type_;
    Signature signature_;
};

template <std::size_t Size>
[[nodiscard]] WiringPoint materialize_fixed_time_series_list(std::span<const WiringPoint> items,
                                                             const TimeSeriesListType &type) {
    auto node = std::make_unique<FreeFixedTimeSeriesListNodeTemplate<Size>>(type);
    const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node));
    return *owned(items);
}

class FreeFixedTimeSeriesListWiringRecord final : public WiringRecord {
    using MaterializeFunction = WiringPoint (*)(std::span<const WiringPoint>, const TimeSeriesListType &);

  public:
    template <std::size_t Size>
    explicit FreeFixedTimeSeriesListWiringRecord(std::array<WiringPoint, Size> items)
        : WiringRecord{argument_type(items)}, items_{items.begin(), items.end()},
          materialize_{
              [](auto values, const auto &type) { return materialize_fixed_time_series_list<Size>(values, type); }} {}

    WiringPoint item(std::size_t index) const { return items_.at(index); }

  private:
    WiringPoint materialize(const TimeSeriesType &target_type) const override {
        const auto &value_type = target_type_of(target_type);
        const auto *list = dynamic_cast<const TimeSeriesListType *>(&value_type);
        if (list == nullptr || list->item_count() != items_.size()) {
            throw WiringError{"a free fixed TSL requires a matching fixed TSL parameter"};
        }

        const auto result = materialize_(items_, *list);
        return materialize_wiring_point(result, target_type);
    }

    static const TimeSeriesType &target_type_of(const TimeSeriesType &type) noexcept {
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
            return reference->target_type();
        }
        return type;
    }

    template <std::size_t Size>
    static const TimeSeriesListType &argument_type(const std::array<WiringPoint, Size> &items) {
        const auto &first = target_type_of(items.front().type());
        auto guarantee = *first.guarantee().value();
        const auto &shape = first.with_guarantee(TimeSeriesGuarantee::ordinary);

        for (const auto item : items | std::views::drop(1)) {
            const auto &candidate = target_type_of(item.type());
            if (&candidate.with_guarantee(TimeSeriesGuarantee::ordinary) != &shape) {
                throw std::invalid_argument{"a free fixed TSL requires compatible item types"};
            }
            guarantee = weakest(guarantee, *candidate.guarantee().value());
        }

        return time_series_list_type(first.with_guarantee(guarantee), Size, TimeSeriesGuarantee::ordinary);
    }

    std::vector<WiringPoint> items_;
    MaterializeFunction materialize_;
};

} // namespace bg::details

namespace bg {

template <typename... TimeSeries> [[nodiscard]] wp<> tsl(wp<TimeSeries>... items) {
    static_assert(sizeof...(TimeSeries) != 0);
    auto points = std::array<WiringPoint, sizeof...(TimeSeries)>{static_cast<WiringPoint>(items)...};
    auto record = std::make_unique<details::FreeFixedTimeSeriesListWiringRecord>(points);
    return wp<>{details::add_wiring_record(std::move(record))};
}

} // namespace bg

#pragma once

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/details/free_time_series_list.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/node/node_template.hpp>
#include <bg/time_series/tsl/tsl_alias_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <array>
#include <charconv>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace bg::details {

template <std::size_t Size> class AliasedFixedTimeSeriesListNodeTemplate final : public NodeTemplate {
    using Storage = aliased_fixed_time_series_list_storage<Size>;

  public:
    explicit AliasedFixedTimeSeriesListNodeTemplate(const TimeSeriesListType &type)
        : representation_{&aliased_fixed_time_series_list_storage_representation<Size>(type)},
          signature_{make_signature(type)} {}

    [[nodiscard]] const Signature &signature() const override { return signature_; }
    [[nodiscard]] EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override {
        return &evaluate;
    }
    [[nodiscard]] EvaluationFunction start_function(const ResolvedSignature &) const noexcept override {
        return &start;
    }
    [[nodiscard]] const TimeSeriesStorageRepresentation *
    output_storage_representation(const ResolvedSignature &, std::span<const ScalarStorageRepresentation *const>,
                                  std::span<const TimeSeriesStorageRepresentation *const>) const noexcept override {
        return representation_;
    }
    [[nodiscard]] bool uses_generic_time_series_input_view(std::size_t) const noexcept override { return true; }
    [[nodiscard]] InputNotificationCallback input_notification(std::size_t input_index) const noexcept override {
        return {&input_changed, input_index};
    }
    [[nodiscard]] std::optional<std::size_t> output_item_source(std::size_t item) const noexcept override {
        return item < Size ? std::optional{item} : std::nullopt;
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

    static Storage &storage(const CompiledNode &node, OutputNotificationContext &context) noexcept {
        return *static_cast<Storage *>(context.runtime_storage_at(node.output_value_offset()));
    }

    static void input_changed(OutputNotificationContext &context, std::size_t node_index, std::size_t input_index,
                              OutputChange) {
        const auto &node = context.compiled_node(node_index);
        auto &value = storage(node, context);
        value.changes.mark(input_index, context.evaluation_time());
    }

    static void start(const CompiledNode &node, void *runtime_storage, EvaluationContext *) noexcept {
        auto *bytes = static_cast<std::byte *>(runtime_storage);
        reinterpret_cast<Storage *>(bytes + node.output_value_offset())->node = &node;
    }

    static void evaluate(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        auto *bytes = static_cast<std::byte *>(runtime_storage);
        const auto &value = *reinterpret_cast<const Storage *>(bytes + node.output_value_offset());
        node.output_last_updated(runtime_storage) = aliased_list_valid(value) ? context->evaluation_time() : MIN_DT;
    }

    const TimeSeriesStorageRepresentation *representation_;
    Signature signature_;
};

template <std::size_t Size>
[[nodiscard]] WiringPoint materialize_fixed_time_series_list(std::span<const WiringPoint> items,
                                                             const TimeSeriesListType &type) {
    auto node = std::make_unique<AliasedFixedTimeSeriesListNodeTemplate<Size>>(type);
    const auto &owned = WiringContext::current().wiring().own_node_template(std::move(node));
    return *owned(items);
}

} // namespace bg::details

namespace bg {

template <std::size_t Size> class FixedTimeSeriesListWiring {
  public:
    explicit FixedTimeSeriesListWiring(std::array<WiringPoint, Size> items)
        : items_{items}, type_{&argument_type(items)} {}

    [[nodiscard]] details::FreeTimeSeriesListArgument wiring_argument() const noexcept {
        return {type_, items_, &details::materialize_fixed_time_series_list<Size>};
    }

  private:
    [[nodiscard]] static const TimeSeriesType &target_type(const TimeSeriesType &type) noexcept {
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
            return reference->target_type();
        }
        return type;
    }

    [[nodiscard]] static const TimeSeriesListType &argument_type(const std::array<WiringPoint, Size> &items) {
        const auto &first = target_type(items.front().type());
        auto guarantee = *first.guarantee().value();
        const auto &shape = first.with_guarantee(TimeSeriesGuarantee::ordinary);

        for (const auto item : items | std::views::drop(1)) {
            const auto &candidate = target_type(item.type());
            if (&candidate.with_guarantee(TimeSeriesGuarantee::ordinary) != &shape) {
                throw std::invalid_argument{"a free time series list requires compatible item types"};
            }
            guarantee = weakest(guarantee, *candidate.guarantee().value());
        }

        return time_series_list_type(first.with_guarantee(guarantee), Size, TimeSeriesGuarantee::ordinary);
    }

    std::array<WiringPoint, Size> items_;
    const TimeSeriesListType *type_;
};

template <typename... TimeSeries>
[[nodiscard]] auto tsl(wp<TimeSeries>... items) -> FixedTimeSeriesListWiring<sizeof...(TimeSeries)> {
    static_assert(sizeof...(TimeSeries) != 0);
    return FixedTimeSeriesListWiring<sizeof...(TimeSeries)>{
        std::array<WiringPoint, sizeof...(TimeSeries)>{static_cast<WiringPoint>(items)...}};
}

} // namespace bg

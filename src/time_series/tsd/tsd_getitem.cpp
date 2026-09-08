#include <bg/time_series/tsd/details/getitem.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/compiled_node.hpp>
#include <bg/graph/node.hpp>
#include <bg/node/node_template.hpp>
#include <bg/scalar/details/allocation_context.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <vector>

namespace bg::details {
namespace {

struct DictionaryItemState {
    const TimeSeriesDictionaryType *requested_source;
    ReferenceAddress source;
    ScalarValue<> *key{};
    bool time_series_key;
};

class DictionaryItemNodeTemplate final : public NodeTemplate {
  public:
    DictionaryItemNodeTemplate(const TimeSeriesDictionaryType &source, bool time_series_key)
        : source_{&source}, time_series_key_{time_series_key}, output_{&output_type(source)},
          requested_source_{&requested_source_type(source, *output_)}, signature_{make_signature()} {}

    const Signature &signature() const override { return signature_; }
    EvaluationFunction evaluation_function(const ResolvedSignature &) const noexcept override { return &evaluate; }
    EvaluationFunction state_destruction_function(const ResolvedSignature &) const noexcept override {
        return &destroy_state;
    }
    NodeStateLayout state_layout(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                                 std::span<const ScalarType *const>) const override {
        return {
            .size = sizeof(DictionaryItemState),
            .alignment = alignof(DictionaryItemState),
            .field_offsets = {},
            .generic_runtime_types = {},
        };
    }
    void construct_initial_state(const NodeStateLayout &, void *storage) const noexcept override {
        std::construct_at(static_cast<DictionaryItemState *>(storage), requested_source_, ReferenceAddress{}, nullptr,
                          time_series_key_);
    }
    bool uses_time_series_input_runtime_type(std::size_t parameter) const noexcept override {
        return parameter == 0 || time_series_key_;
    }
    bool uses_scalar_input_runtime_type(std::size_t parameter) const noexcept override {
        return parameter == 1 && !time_series_key_;
    }
    bool uses_output_runtime_type() const noexcept override { return true; }

    const TimeSeriesRuntimeType *
    output_runtime_type(const ResolvedSignature &, std::span<const ScalarRuntimeType *const>,
                        std::span<const TimeSeriesRuntimeType *const> input_runtime_types) const override {
        const auto &source_reference = static_cast<const TimeSeriesReferenceRuntimeType &>(*input_runtime_types[0]);
        const auto &source_dictionary =
            static_cast<const TimeSeriesDictionaryRuntimeType &>(source_reference.target_runtime_type());
        const auto *target = &source_dictionary.item_runtime_type();
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceRuntimeType *>(target)) {
            target = &reference->target_runtime_type();
        }
        return &time_series_reference_runtime_type(*output_, *target);
    }

  private:
    static const TimeSeriesReferenceType &output_type(const TimeSeriesDictionaryType &source) {
        const auto *target = &source.value_type();
        if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(target)) {
            target = &reference->target_type();
        }
        return time_series_reference_type(*target, TimeSeriesGuarantee::deduplicated);
    }

    static const TimeSeriesDictionaryType &requested_source_type(const TimeSeriesDictionaryType &source,
                                                                 const TimeSeriesReferenceType &output) {
        if (dynamic_cast<const TimeSeriesReferenceType *>(&source.value_type()) != nullptr) {
            return source;
        }
        return time_series_dictionary_type(source.key_type(), output, *source.guarantee().value());
    }

    [[nodiscard]] Signature make_signature() const {
        const auto &reference = time_series_reference_type(*source_, TimeSeriesGuarantee::ordinary);
        auto parameters = std::vector<Parameter>{};
        parameters.push_back({util::symbol{"dictionary"}, &reference, active, optional});
        parameters.push_back({util::symbol{"key"},
                              time_series_key_ ? static_cast<const Type *>(&time_series_value_type(
                                                     source_->key_type(), TimeSeriesGuarantee::ordinary))
                                               : static_cast<const Type *>(&source_->key_type()),
                              active, optional, false});
        return {util::symbol{"getitem"}, std::move(parameters), *output_};
    }

    static ScalarData key(const DictionaryItemState &state, const CompiledNode &node, const Node &runtime_node) {
        if (!state.time_series_key) {
            return {node.scalar_input_runtime_type(1, 0), node.parameters()[1].scalar_data()};
        }

        const auto &runtime_type =
            static_cast<const TimeSeriesValueRuntimeType &>(runtime_node.time_series_input_runtime_type(1, 1));
        return {runtime_type.scalar_runtime_type(), runtime_node.input_value(1)};
    }

    static bool key_valid(const DictionaryItemState &state, const Node &node) noexcept {
        return !state.time_series_key || node.input_last_updated(1) > MIN_DT;
    }

    static void remove_subscription(DictionaryItemState &state, const CompiledNode &node,
                                    EvaluationContext &context) noexcept {
        if (!state.source || state.key == nullptr) {
            return;
        }
        remove_reference_dictionary_key_subscriber(state.source, *state.key, context.runtime_graph(),
                                                   static_cast<std::uint32_t>(node.parameters()[0].value_offset()));
        state.source = nullptr;
    }

    static void replace_key(DictionaryItemState &state, const ScalarData &new_key) {
        auto &memory = current_scalar_memory_resource();
        if (state.key == nullptr) {
            auto *storage = memory.allocate(sizeof(ScalarValue<>), alignof(ScalarValue<>));
            state.key = std::construct_at(static_cast<ScalarValue<> *>(storage), Scalar<>::from(new_key), memory);
            return;
        }
        auto replacement = ScalarValue<>{Scalar<>::from(new_key), memory};
        *state.key = std::move(replacement);
    }

    [[nodiscard]] static ReferenceAddress select(ReferenceAddress dictionary, const ScalarData &key) {
        const auto target = reference_target(dictionary);
        const auto &runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*target.runtime_type);
        const auto id = runtime_type.find_item(target.value, key);
        if (id == util::occupancy_index::npos) {
            return {};
        }

        const auto item = runtime_type.item(target.value, id);
        return item.last_updated > MIN_DT ? *static_cast<const ReferenceAddress *>(item.value) : ReferenceAddress{};
    }

    static void evaluate(const CompiledNode &compiled_node, void *runtime_storage, EvaluationContext *context) {
        const auto node = Node{compiled_node, runtime_storage};
        auto &state = *static_cast<DictionaryItemState *>(node.state());
        auto &output = *static_cast<ReferenceAddress *>(node.output_value());

        if (node.input_last_updated(0) <= MIN_DT || !key_valid(state, node)) {
            remove_subscription(state, compiled_node, *context);
            output = nullptr;
            node.output_last_updated() = MIN_DT;
            return;
        }

        const auto selected_key = key(state, compiled_node, node);
        const auto source = *static_cast<const ReferenceAddress *>(node.input_value(0));
        if (!source) {
            remove_subscription(state, compiled_node, *context);
            output = nullptr;
            node.output_last_updated() = MIN_DT;
            return;
        }

        const auto dictionary = reference_representation(source, *state.requested_source);
        const auto subscription_unchanged =
            state.source == dictionary && state.key != nullptr && state.key->equals(selected_key);
        if (!subscription_unchanged) {
            remove_subscription(state, compiled_node, *context);
            replace_key(state, selected_key);
            add_reference_dictionary_key_subscriber(
                dictionary, *state.key, context->runtime_graph(),
                static_cast<std::uint32_t>(compiled_node.parameters()[0].value_offset()),
                static_cast<std::uint32_t>(context->node_index()));
            state.source = dictionary;
        }

        const auto selected = select(dictionary, selected_key);
        if (output == selected) {
            return;
        }
        output = selected;
        node.output_last_updated() = selected ? context->evaluation_time() : MIN_DT;
    }

    static void destroy_state(const CompiledNode &node, void *runtime_storage, EvaluationContext *context) noexcept {
        auto &state = *static_cast<DictionaryItemState *>(Node{node, runtime_storage}.state());
        remove_subscription(state, node, *context);
        if (state.key != nullptr) {
            auto &memory = current_scalar_memory_resource();
            std::destroy_at(state.key);
            memory.deallocate(state.key, sizeof(ScalarValue<>), alignof(ScalarValue<>));
        }
        std::destroy_at(&state);
    }

    const TimeSeriesDictionaryType *source_;
    bool time_series_key_;
    const TimeSeriesReferenceType *output_;
    const TimeSeriesDictionaryType *requested_source_;
    Signature signature_;
};

} // namespace

const NodeTemplate &dictionary_item_node_template(const TimeSeriesDictionaryType &source, bool time_series_key) {
    struct Entry {
        const TimeSeriesDictionaryType *source;
        bool time_series_key;
        std::unique_ptr<DictionaryItemNodeTemplate> node;
    };

    static auto mutex = std::mutex{};
    static auto entries = std::vector<Entry>{};

    const auto lock = std::lock_guard{mutex};
    const auto found = std::ranges::find_if(entries, [&](const auto &entry) {
        return entry.source == &source && entry.time_series_key == time_series_key;
    });
    if (found != entries.end()) {
        return *found->node;
    }

    auto node = std::make_unique<DictionaryItemNodeTemplate>(source, time_series_key);
    auto &result = *node;
    entries.push_back({&source, time_series_key, std::move(node)});
    return result;
}

} // namespace bg::details

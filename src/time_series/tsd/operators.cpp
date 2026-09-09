#include <bg/time_series/tsd/operators.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/type/time_series_reference_type.hpp>

#include <span>

namespace bg::details {
namespace {

using Key = ScalarVariable<"K", Hashable>;
using Item = TimeSeriesVariable<"ITEM">;
using Dictionary = REF<TSD<Key, REF<Item>>>;

struct DictionaryItemState {
    DictionaryKeySubscription subscription;
};

const TimeSeriesDictionaryType &requested_dictionary_type(const In<Dictionary, optional> &dictionary,
                                                          const Out<REF<Item>> &output) {
    const auto target = reference_target(dictionary.target());
    const auto &dictionary_runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*target.runtime_type);
    const auto &dictionary_type =
        static_cast<const TimeSeriesDictionaryType &>(dictionary_runtime_type.type());
    if (dynamic_cast<const TimeSeriesReferenceRuntimeType *>(&dictionary_runtime_type.item_runtime_type()) !=
        nullptr) {
        return dictionary_type;
    }

    const auto &item_reference = static_cast<const TimeSeriesReferenceType &>(output.runtime_type().type());
    return time_series_dictionary_type(dictionary_type.key_type(), item_reference,
                                       *dictionary_type.guarantee().value());
}

ReferenceAddress dictionary_representation(const In<Dictionary, optional> &dictionary,
                                           const Out<REF<Item>> &output) {
    return reference_representation(dictionary.target(), requested_dictionary_type(dictionary, output));
}

ReferenceAddress selected_item(ReferenceAddress dictionary, const ScalarData &key) {
    const auto target = reference_target(dictionary);
    const auto &runtime_type = static_cast<const TimeSeriesDictionaryRuntimeType &>(*target.runtime_type);
    const auto id = runtime_type.find_item(target.value, key);
    if (id == util::occupancy_index::npos) {
        return {};
    }

    const auto item = runtime_type.item(target.value, id);
    return item.last_updated > MIN_DT ? *static_cast<const ReferenceAddress *>(item.value) : ReferenceAddress{};
}

void remove_subscription(State<DictionaryItemState> state, EvaluationContext &context) noexcept {
    remove_reference_dictionary_key_subscriber(state->subscription, context.runtime_graph(),
                                               static_cast<std::uint32_t>(context.node_index()));
    state->subscription = {};
}

void evaluate_getitem(const In<Dictionary, optional> &dictionary, const ScalarData &key,
                      State<DictionaryItemState> state, const Out<REF<Item>> &output,
                      EvaluationContext &context) {
    if (!dictionary.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    const auto source = dictionary_representation(dictionary, output);
    if (!reference_dictionary_key_subscription_matches(state->subscription, source, key)) {
        remove_subscription(state, context);
        state->subscription = add_reference_dictionary_key_subscriber(
            source, key, context.runtime_graph(), static_cast<std::uint32_t>(context.node_index()));
    }

    const auto selected = selected_item(source, key);
    if (selected) {
        output.set_target(selected);
    } else {
        output.invalidate();
    }
}

struct tsd_getitem_scalar {
    In<Dictionary, optional> dictionary;
    Scalar<Key> key;
    State<DictionaryItemState> state;
    Out<REF<Item>> output;

    void evaluate(EvaluationContext *context) const {
        evaluate_getitem(dictionary, key, state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

struct tsd_getitem_time_series {
    In<Dictionary, optional> dictionary;
    In<TS<Key>, optional> key;
    State<DictionaryItemState> state;
    Out<REF<Item>> output;

    void evaluate(EvaluationContext *context) const {
        if (!key.valid()) {
            remove_subscription(state, *context);
            output.invalidate();
            return;
        }

        const auto selected_key = key.value_view();
        evaluate_getitem(dictionary, selected_key, state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

} // namespace

void register_time_series_dict_operators(Operator &getitem) {
    getitem.overload(bg::node<tsd_getitem_scalar>());
    getitem.overload(bg::node<tsd_getitem_time_series>());
}

} // namespace bg::details

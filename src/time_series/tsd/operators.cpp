#include <bg/time_series/tsd/operators.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

namespace bg::details {
namespace {

using K = ScalarVariable<"K", Hashable>;
using ITEM = TimeSeriesVariable<"ITEM">;

struct DictionaryItemState {
    DictionaryKeySubscription subscription;
};

void remove_subscription(State<DictionaryItemState> state, EvaluationContext &context) noexcept {
    remove_reference_dictionary_key_subscriber(state->subscription, context.runtime_graph(),
                                               static_cast<std::uint32_t>(context.node_index()));
    state->subscription = {};
}

void evaluate_getitem(const In<REF<TSD<K, ITEM>>, optional> &dictionary, const ScalarData &key,
                      State<DictionaryItemState> state, const Out<REF<ITEM>> &output, EvaluationContext &context) {
    if (!dictionary.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    const auto address = dictionary.target();
    if (!reference_dictionary_key_subscription_matches(state->subscription, address, key)) {
        remove_subscription(state, context);
        state->subscription = add_reference_dictionary_key_subscriber(address, key, context.runtime_graph(),
                                                                      static_cast<std::uint32_t>(context.node_index()));
    }

    const auto selected = reference_dictionary_key_subscription_target(state->subscription);
    if (selected) {
        output.set_target(selected);
    } else {
        output.invalidate();
    }
}

struct tsd_getitem_scalar {
    In<REF<TSD<K, ITEM>>, optional> dictionary;
    Scalar<K> key;
    State<DictionaryItemState> state;
    Out<REF<ITEM>> output;

    void evaluate(EvaluationContext *context) const { evaluate_getitem(dictionary, key, state, output, *context); }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

struct tsd_getitem_time_series {
    In<REF<TSD<K, ITEM>>, optional> dictionary;
    In<TS<K>, optional> key;
    State<DictionaryItemState> state;
    Out<REF<ITEM>> output;

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

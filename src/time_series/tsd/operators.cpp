#include <bg/time_series/tsd/operators.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_key_subscription.hpp>

namespace bg::details {
namespace {

using K = ScalarVariable<"K", Hashable>;
using ITEM = TimeSeriesVariable<"ITEM">;

struct TsdItemState {
    TsdKeySubscription subscription;
};

void remove_subscription(State<TsdItemState> state, EvaluationContext &context) noexcept {
    remove_tsd_key_subscriber(state->subscription, context.runtime_graph(),
                              static_cast<std::uint32_t>(context.node_index()));
    state->subscription = {};
}

void evaluate_getitem(const In<REF<TSD<K, ITEM>>, optional> &tsd, const ScalarData &key, State<TsdItemState> state,
                      const Out<REF<ITEM>> &output, EvaluationContext &context) {
    if (!tsd.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    const auto reference = tsd.value();
    if (!tsd_key_subscription_matches(state->subscription, reference, key)) {
        remove_subscription(state, context);
        state->subscription = add_tsd_key_subscriber(reference, key, context.runtime_graph(),
                                                     static_cast<std::uint32_t>(context.node_index()));
    }

    const auto selected = tsd_key_subscription_reference(state->subscription);
    if (selected) {
        output.set_value(selected);
    } else {
        output.invalidate();
    }
}

struct tsd_getitem_scalar {
    In<REF<TSD<K, ITEM>>, optional> tsd;
    Scalar<K> key;
    State<TsdItemState> state;
    Out<REF<ITEM>> output;

    void evaluate(EvaluationContext *context) const {
        evaluate_getitem(tsd, scalar_data(key), state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

struct tsd_getitem_time_series {
    In<REF<TSD<K, ITEM>>, optional> tsd;
    In<TS<K>, optional> key;
    State<TsdItemState> state;
    Out<REF<ITEM>> output;

    void evaluate(EvaluationContext *context) const {
        if (!key.valid()) {
            remove_subscription(state, *context);
            output.invalidate();
            return;
        }

        const auto selected_key = key.value_view();
        evaluate_getitem(tsd, scalar_data(selected_key), state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

} // namespace

void register_time_series_dict_operators(Operator &getitem) {
    getitem.overload(bg::node<tsd_getitem_scalar>());
    getitem.overload(bg::node<tsd_getitem_time_series>());
}

} // namespace bg::details

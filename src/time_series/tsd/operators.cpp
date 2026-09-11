#include <bg/time_series/tsd/operators.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_key_subscription.hpp>

namespace bg::details {
namespace {

using K = ScalarVariable<"K", Hashable>;
using V = TimeSeriesVariable<"V">;
inline constexpr auto G = guarantee_variable<"G">;

struct TsdItemState {
    TsdKeySubscription subscription;
};

void remove_subscription(State<TsdItemState> state, EvaluationContext &context) noexcept {
    remove_tsd_key_subscriber(state->subscription, context.runtime_graph(),
                              static_cast<std::uint32_t>(context.node_index()));
    state->subscription = {};
}

void update_value_item(
    const In<REF<TSD<K, V, G>, G>, optional> &tsd, Scalar<> key, bool key_modified, State<TsdItemState> state,
    const Out<REF<V, G>> &output, EvaluationContext &context) {
    if (!tsd.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    if (!state->subscription || tsd.modified() || key_modified) {
        remove_subscription(state, context);
        state->subscription = add_tsd_key_subscriber(tsd.value(), key, context.runtime_graph(),
                                                     static_cast<std::uint32_t>(context.node_index()));
    }

    const auto value = tsd_key_subscription_value(state->subscription);
    if (value) {
        output.set_value(value);
    } else {
        output.invalidate();
    }
}

struct tsd_value_getitem_scalar {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    Scalar<K> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        update_value_item(tsd, Scalar<>::from(key), false, state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

struct tsd_value_getitem_time_series {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    In<TS<K, G>, optional> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        if (!key.valid()) {
            remove_subscription(state, *context);
            output.invalidate();
            return;
        }

        const auto selected_key = key.value_view();
        update_value_item(tsd, Scalar<>::from(selected_key), key.modified(), state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

void update_reference_item(
    const In<REF<TSD<K, REF<V, G>, G>, G>, optional> &tsd, Scalar<> key, bool key_modified,
    State<TsdItemState> state, const Out<REF<V, G>> &output, EvaluationContext &context) {
    if (!tsd.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    if (!state->subscription || tsd.modified() || key_modified) {
        remove_subscription(state, context);
        state->subscription = add_tsd_key_subscriber(tsd.value(), key, context.runtime_graph(),
                                                     static_cast<std::uint32_t>(context.node_index()));
    }

    const auto value = tsd_key_subscription_value(state->subscription);
    if (!value) {
        output.invalidate();
        return;
    }

    output.set_value(value);
    if (tsd_key_subscription_is_final_and_valid(state->subscription)) {
        remove_subscription(state, context);
    }
}

struct tsd_reference_getitem_scalar {
    In<REF<TSD<K, REF<V, G>, G>, G>, optional> tsd;
    Scalar<K> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        update_reference_item(tsd, Scalar<>::from(key), false, state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

struct tsd_reference_getitem_time_series {
    In<REF<TSD<K, REF<V, G>, G>, G>, optional> tsd;
    In<TS<K, G>, optional> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        if (!key.valid()) {
            remove_subscription(state, *context);
            output.invalidate();
            return;
        }

        const auto selected_key = key.value_view();
        update_reference_item(tsd, Scalar<>::from(selected_key), key.modified(), state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept { remove_subscription(state, *context); }
};

} // namespace

void register_time_series_dict_operators(Operator &getitem) {
    getitem.overload(bg::node<tsd_value_getitem_scalar>());
    getitem.overload(bg::node<tsd_value_getitem_time_series>());
    getitem.overload(bg::node<tsd_reference_getitem_scalar>());
    getitem.overload(bg::node<tsd_reference_getitem_time_series>());
}

} // namespace bg::details

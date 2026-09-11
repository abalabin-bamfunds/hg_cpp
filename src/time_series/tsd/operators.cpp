#include <bg/time_series/tsd/operators.hpp>

#include <bg/evaluation_context.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsd/tsd_key_subscription.hpp>

namespace bg::details {
namespace {

using K = ScalarVariable<"K", Hashable>;
using V = TimeSeriesVariable<"V">;
inline constexpr auto G = guarantee_variable<"G">;

struct TsdItemState {
    Reference subscribed_tsd;
};

void release_output_item(const Out<REF<V, G>> &output) noexcept {
    if (output.valid()) {
        release_tsd_item_reference(output.value());
    }
}

void remove_subscription(State<TsdItemState> state, EvaluationContext &context) noexcept {
    if (state->subscribed_tsd) {
        remove_tsd_key_subscriber(state->subscribed_tsd, context.runtime_graph(),
                                  static_cast<std::uint32_t>(context.node_index()));
        state->subscribed_tsd = {};
    }
}

void replace_subscription(Reference tsd, Scalar<> key, State<TsdItemState> state, EvaluationContext &context) {
    remove_subscription(state, context);
    add_tsd_key_subscriber(tsd, key, context.runtime_graph(), static_cast<std::uint32_t>(context.node_index()));
    state->subscribed_tsd = tsd;
}

template <typename Item>
In<TSD<K, Item, G>> dictionary_input(const In<REF<TSD<K, Item, G>, G>, optional> &tsd) {
    const auto resolved = resolve_reference(tsd.value());
    return {*resolved.last_updated, resolved.storage, *resolved.runtime_type, tsd.modified()};
}

void update_value_item(const In<REF<TSD<K, V, G>, G>, optional> &tsd, const Scalar<K> &key,
                       bool key_modified, State<TsdItemState> state, const Out<REF<V, G>> &output,
                       EvaluationContext &context) {
    if (!tsd.valid()) {
        remove_subscription(state, context);
        release_output_item(output);
        output.invalidate();
        return;
    }

    if (!state->subscribed_tsd || tsd.modified() || key_modified) {
        release_output_item(output);
        output.invalidate();
        replace_subscription(tsd.value(), Scalar<>::from(key), state, context);
    }

    const auto dictionary = dictionary_input(tsd);
    if (!dictionary.contains(key) || !dictionary[key].valid()) {
        release_output_item(output);
        output.invalidate();
        return;
    }
    if (!output.valid()) {
        output.set_value(acquire_tsd_item_reference(tsd.value(), Scalar<>::from(key)));
    }
}

struct tsd_value_getitem_scalar {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    Scalar<K> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const { update_value_item(tsd, key, false, state, output, *context); }

    void finalize(EvaluationContext *context) const noexcept {
        remove_subscription(state, *context);
        release_output_item(output);
    }
};

struct tsd_value_getitem_time_series {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    In<TS<K, G>, optional> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        if (!key.valid()) {
            remove_subscription(state, *context);
            release_output_item(output);
            output.invalidate();
            return;
        }

        const auto selected_key = key.value_view();
        update_value_item(tsd, selected_key, key.modified(), state, output, *context);
    }

    void finalize(EvaluationContext *context) const noexcept {
        remove_subscription(state, *context);
        release_output_item(output);
    }
};

bool is_final(const In<REF<V, G>> &item) {
    const auto guarantee = item.runtime_type().type().guarantee().value();
    return guarantee && satisfies(*guarantee, TimeSeriesGuarantee::final);
}

void update_reference_item(const In<REF<TSD<K, REF<V, G>, G>, G>, optional> &tsd, const Scalar<K> &key,
                           bool key_modified, State<TsdItemState> state, const Out<REF<V, G>> &output,
                           EvaluationContext &context) {
    if (!tsd.valid()) {
        remove_subscription(state, context);
        output.invalidate();
        return;
    }

    if (!state->subscribed_tsd || tsd.modified() || key_modified) {
        replace_subscription(tsd.value(), Scalar<>::from(key), state, context);
    }

    const auto dictionary = dictionary_input(tsd);
    if (!dictionary.contains(key)) {
        output.invalidate();
        return;
    }

    const auto item = dictionary[key];
    if (!item.valid()) {
        output.invalidate();
        return;
    }

    output.set_value(item.value());
    if (is_final(item)) {
        remove_subscription(state, context);
    }
}

struct tsd_reference_getitem_scalar {
    In<REF<TSD<K, REF<V, G>, G>, G>, optional> tsd;
    Scalar<K> key;
    State<TsdItemState> state;
    Out<REF<V, G>> output;

    void evaluate(EvaluationContext *context) const {
        update_reference_item(tsd, key, false, state, output, *context);
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
        update_reference_item(tsd, selected_key, key.modified(), state, output, *context);
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

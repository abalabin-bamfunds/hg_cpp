#include <bg/time_series/tsd/operators.hpp>

#include <bg/graph.hpp>
#include <bg/graph/operator.hpp>
#include <bg/node.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>

namespace bg::details {
namespace {

using K = ScalarVariable<"K", Hashable>;
using V = TimeSeriesVariable<"V">;
inline constexpr auto G = guarantee_variable<"G">;

struct tsd_value_getitem_scalar {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    Scalar<K> key;
    In<REF<V, G>, dynamic_binding, optional> item;
    Out<REF<V, G>> output;

    void evaluate() const {
        item.bind(tsd[key]);
        output.copy(item);
    }
};

struct tsd_value_getitem_time_series {
    In<REF<TSD<K, V, G>, G>, optional> tsd;
    In<TS<K, G>, optional> key;
    In<REF<V, G>, dynamic_binding, optional> item;
    Out<REF<V, G>> output;

    void evaluate() const {
        if (!key.valid()) {
            item.unbind();
            output.invalidate();
            return;
        }

        item.bind(tsd[key.value_view()]);
        output.copy(item);
    }
};

template <typename KEY, typename VALUE, auto GUARANTEE> struct tsd_getitem_tss {
    In<TSD<KEY, REF<VALUE, GUARANTEE>, GUARANTEE>, optional> dictionary;
    In<TSS<KEY>, optional> keys;
    Out<TSD<KEY, REF<VALUE, GUARANTEE>>> output;

    void evaluate() const {
        if (!keys.valid()) {
            output.invalidate();
            return;
        }

        if (!output.valid()) {
            output.clear();
        }

        keys.for_each_removed([&](const auto &key) { output.erase(key); });
        if (!dictionary.valid()) {
            keys.for_each([&](const auto &key) { output[key].invalidate(); });
            return;
        }

        if (dictionary.became_valid()) {
            keys.for_each([&](const auto &key) { update(key); });
            return;
        }

        keys.for_each_added([&](const auto &key) { update(key); });
        dictionary.for_each_removed([&](const auto &key) {
            if (keys.contains(key)) {
                output[key].invalidate();
            }
        });
        dictionary.for_each_modified([&](const auto &key) { update_if_selected(key); });
    }

    void update(const Scalar<KEY> &key) const {
        const auto output_item = output[key];
        if (!dictionary.contains(key)) {
            output_item.invalidate();
            return;
        }

        const auto item = dictionary[key];
        if (!item.valid()) {
            output_item.invalidate();
            return;
        }
        if (!output_item.valid() || output_item.value() != item.value()) {
            output_item.copy(item);
        }
    }

    void update_if_selected(const Scalar<KEY> &key) const {
        if (keys.contains(key)) {
            update(key);
        }
    }
};

} // namespace

void register_time_series_dict_operators(Operator &getitem) {
    getitem.overload(bg::node<tsd_value_getitem_scalar>());
    getitem.overload(bg::node<tsd_value_getitem_time_series>());
    getitem.overload(bg::graph([](wp<TSD<K, V, G>, optional> dictionary, wp<TSS<K>> keys) {
        static const auto select = bg::node<tsd_getitem_tss<K, V, G>>();
        return select(dictionary.as<TSD<K, REF<V, G>, G>>(), keys);
    }));
    getitem.overload(bg::graph([](wp<TSD<K, REF<V, G>, G>, optional> dictionary, wp<TSS<K>> keys) {
        static const auto select = bg::node<tsd_getitem_tss<K, V, G>>();
        return select(dictionary, keys);
    }));
}

} // namespace bg::details

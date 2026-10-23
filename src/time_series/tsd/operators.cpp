#include <bg/time_series/tsd/operators.hpp>

#include <bg/graph/operator.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/operators.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>

namespace bg::details {
namespace {

using namespace bg::literals;

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

struct tsd_reference_getitem_scalar {
    In<REF<TSD<K, REF<V, G>, G>, G>, optional> tsd;
    Scalar<K> key;
    In<REF<V, G>, dynamic_binding, optional> item;
    Out<REF<V, G>> output;

    void evaluate() const {
        item.bind(tsd[key]);
        output.copy(item);
    }
};

struct tsd_reference_getitem_time_series {
    In<REF<TSD<K, REF<V, G>, G>, G>, optional> tsd;
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

} // namespace

void register_time_series_dict_operators(Operator &getitem) {
    getitem.overload(bg::node<tsd_value_getitem_scalar>());
    getitem.overload(bg::node<tsd_value_getitem_time_series>());
    getitem.overload(bg::node<tsd_reference_getitem_scalar>());
    getitem.overload(bg::node<tsd_reference_getitem_time_series>());
    getitem.overload(bg::graph([](wp<TSD<K, V>, optional> dictionary, wp<TSS<K>> keys) -> wp<TSD<K, V>> {
        return map(operators::getitem, pass_through(dictionary), keys, "__key_arg__"_n = 1)
            .template as<TSD<K, V>>();
    }));
}

} // namespace bg::details

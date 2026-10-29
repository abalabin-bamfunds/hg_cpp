# Collection operators

Use collection operators to query or summarize current items. They do not accumulate
successive input events. The code fragments use existing collection connections and
direct Python imports or C++ namespace imports.

## Collection reductions

`sum_(values)`, `min_(values)`, `max_(values)`, and `mean(values)` calculate over the
current items of a numeric fixed-size TSL, TSD, or TSS. They accept `i64` and `double`
items. Sums and extrema retain the item scalar type. `mean` returns `double` and
accumulates in `double`; integer sums use the [wrapping
policy](library-numeric.md#arithmetic).

An ordinary call requires a valid collection and valid immediate items. `opt(values)`
selects an optional overload that skips invalid items:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import opt
from bg.operators import sum_

complete_total = sum_(values)
available_total = sum_(opt(values))
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto complete_total = sum_(values);
auto available_total = sum_(opt(values));
```
:::
::::

With no contributing items, sum publishes zero and the other reductions invalidate their
output. An invalid optional collection has no contributing items. These nodes do not
schedule on start; an input change schedules the calculation. Skipped cycles preserve
the previous result.

Each evaluation reads the complete current collection. Item updates, invalidations,
additions, removals, and reference changes therefore contribute to the next result
without retaining previous item values. Ordinary outputs publish on each permitted
evaluation, including equal results. Extrema use ordinary floating-point comparisons in
enumeration order. TSD and TSS enumeration order is unspecified.

The `all_valid` sum propagates the observed collection guarantee. Optional sum always
has an ordinary output and publishes on each evaluation, including equal results. For
example, binding an invalid set and then observing `{0}` publishes `0` twice.

The other reductions retain ordinary, deduplicated, and final guarantees, and limit
const to final because an empty collection has no result. This applies to both overload
versions. Each evaluation takes O(N) work and O(1) auxiliary storage.

## Boolean reductions

`all_(values)` and `any_(values)` reduce a Boolean fixed-size TSL, TSD, or TSS. `all_`
tests whether every contributing value is true; `any_` tests whether at least one is
true. They accept one collection argument. A required overload needs a valid root and
valid immediate items. `opt(values)` selects an overload that skips invalid items and
treats an invalid root as empty:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import opt
from bg.operators import all_, any_

complete = all_(values)
available = any_(opt(values))
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto complete = all_(values);
auto available = any_(opt(values));
```
:::
::::

Use `valid_items(values)` to exclude invalid TSD items while still requiring root
validity, following the [valid-item rules](input-views.md#collection-views). It can also
be combined with `opt()`.

With no contributing items, `all_` publishes true and `any_` publishes false. Input
changes recalculate the result, including item validity changes and removals. Neither
operator schedules on start. Ordinary outputs publish equal results on separate
evaluations. Required overloads propagate the observed collection guarantee; optional
overloads produce ordinary outputs.

Each calculation retains no state. TSL and TSD evaluation takes O(N) work in the worst
case; TSS uses one Boolean membership lookup.


## Collection queries

`keys_(dictionary)` returns a TSS with the dictionary's current keys. It projects
the existing key-set series, so value updates do not update the result. It follows
dictionary reference changes and preserves the dictionary guarantee.

`keys_(valid_items(dictionary))` returns only keys whose immediate items are valid.
It updates when an item becomes valid or invalid. An invalid dictionary root produces
an invalid result; revalidation rebuilds the key set. `len_` and `is_empty` accept the
same valid-item view and query its current keys. These filtered queries produce
ordinary outputs.

`keys_where_true(mask)` returns the keys whose TSD Boolean items are valid and true.
It updates when an item becomes false or invalid, or when a key is added or removed.

`len_(value)` returns an `i64` length. It counts current TSS membership, physical TSD
keys, or the items in `TS<List<T>>`, `TS<Set<T>>`, and `TS<Dict<K, V>>`. Use
`len_(valid_items(dictionary))` to count only valid immediate TSD items. For fixed TSL
and TSB, it returns the declared item count as a wiring-time constant, independent of
item validity. These fixed-size queries add no runtime query node. Sparse TSL length is
not supplied. `is_empty(dictionary)` tests whether the dictionary has any physical keys;
`is_empty(valid_items(dictionary))` tests whether it has any valid immediate items.

`contains_(collection, key)` tests current membership in TSS, TSD, or a scalar list,
set, or dictionary time series. Dictionaries test keys. The key can be a scalar or a
time series. List membership requires equality; set and dictionary lookup also requires
hashing.

TSS and TSD queries subscribe to the selected key. Unrelated key changes do not schedule
the query. Ordinary TSD membership ignores item validity and value updates. Use
`contains_(valid_items(collection), key)` to require a valid immediate item, following
the [valid-item rules](input-views.md#collection-views). A valid collection with no
matching key produces `false`; an invalid collection has no membership result. A time
series key tick recomputes membership, including an equal-valued ordinary tick.

String and symbol overloads count Unicode code points with `len_` and test substrings with
`contains_`, including embedded NUL bytes. Both substring operands have the same scalar
type. These overloads accept scalars and time series.

Runtime queries require valid time series inputs and propagate their weakest observed
guarantee. An initially invalid collection does not produce a result. Ordinary inputs
can publish equal query results on successive changes; deduplicated outputs suppress
equal results.

Use named calls in Python. Its `len()` and `in` syntax cannot return wiring points.
Python uses `tuple[T, ...]` for scalar lists. Scalar set annotations are not currently
exposed.

## Time series set operations

Two `TSS<K>` inputs support union (`bit_or`, `|`), intersection (`bit_and`, `&`),
difference (`sub`, `-`), and symmetric difference (`bit_xor`, `^`). Both inputs must
have the same key type. `eq` and `ne` compare membership; `is_empty` tests whether a set
has no members.

Two `TSD<K, TS_T>` inputs support the same set operators. `&` selects keys present in
both dictionaries, and `-` selects keys present only in the left dictionary. `|` selects
keys present in either dictionary, and `^` selects keys present in exactly one. The
result contains references to the selected items. When both inputs contain a selected
key, `|` uses the left item, even if that physical item is invalid. Use
`valid_items(lhs) | rhs` to exclude invalid left items so a valid right item can supply
that key. These operators accept `valid_items` on either or both inputs; that view
excludes invalid immediate items from membership. Both TSD roots must be valid. If
either root is invalid, the result is invalid until both roots are valid again.

TSS inputs are required by default. An invalid required input invalidates the
result. Revalidation computes the result from the current sets.

For TSS set operators, `opt()` selects overloads that treat invalid inputs as empty sets.
Marking either binary operand selects that behavior for both operands. Optional
overloads evaluate on input changes, without startup scheduling. TSD set operators do
not have optional-root overloads.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from bg import opt
from bg.operators import is_empty

shared = lhs & rhs
available = opt(lhs) | opt(rhs)
empty = is_empty(available)
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto shared = lhs & rhs;
auto available = opt(lhs) | opt(rhs);
auto empty = is_empty(available);
```
:::
::::

TSS set results publish only actual membership changes, including a first valid empty
result. Updating one operand without changing the result membership does not publish
another set tick.

Required TSS overloads propagate the weakest observed input guarantee. Optional TSS
overloads produce ordinary outputs. Boolean outputs publish on each evaluation unless their
resolved guarantee suppresses an equal result.

TSD set results expose ordinary dictionary and item-reference guarantees. Selected
reference targets retain their guarantees. Target updates reach consumers without
reevaluating the routing node.

## Dictionary transforms

Key-routing transforms change keys or grouping without copying item values.
Their reference outputs identify source items. Target updates reach downstream
consumers without evaluating the transform again.

In this table, `V` denotes a time series type:

| Operator | Input | Output |
| --- | --- | --- |
| `rekey(ts, new_keys)` | `TSD<K, V>`, `TSD<K, TS<K1>>` | `TSD<K1, REF<V>>` |
| `partition(ts, partitions)` | `TSD<K, V>`, `TSD<K, TS<P>>` | `TSD<P, TSD<K, REF<V>>>` |
| `unpartition(ts)` | `TSD<P, TSD<K, V>>` | `TSD<K, REF<V>>` |
| `flip_keys(ts)` | `TSD<K, TSD<K1, V>>` | `TSD<K1, TSD<K, REF<V>>>` |
| `collapse_keys(ts)` | `TSD<K, TSD<K1, V>>` | `TSD<Tuple<K, K1>, REF<V>>` |
| `uncollapse_keys(ts)` | `TSD<Tuple<K, K1>, V>` | `TSD<K, TSD<K1, REF<V>>>` |

Existing reference items retain their targets without adding another reference
layer. Invalid item targets retain their keys. An absent or invalid mapping item
in `rekey` or `partition` removes that source item's route. Source and mapping
changes both update routes. An input becoming invalid through reference rebinding
removes its routes. A valid empty source establishes a valid empty result.

`partition`, `flip_keys`, and `uncollapse_keys` remove empty output groups by
default. Pass `remove_empty=false` to retain groups after their last item is
removed. Python spells the value `False`.

`flip(ts, unique=true)` reverses a `TSD<K, TS<V>>` into `TSD<V, TS<K>>`.
Keys and values must support hashing. An invalid source item has no reverse entry.
With `unique=false`, the result is `TSD<V, TSS<K>>`: each value groups all source
keys that hold it. Removing the last key removes the group. The `unique` argument
is a wiring-time scalar; Python uses `True` and `False`.

`rekey` requires distinct destination keys. Unique `flip` requires distinct valid
values. `unpartition` requires distinct item keys across its input groups. These
requirements are not checked during evaluation. Removals run before assignments.
Each removal erases its destination, including a destination overwritten by another
source. Later assignments win in update iteration order. No replacement source is
selected automatically; a later assignment can restore the destination.

Key collapse and expansion support exactly two key levels. Python represents
`Tuple<K, K1>` with `tuple[K, K1]`.

## getitem and getattr

`collection[key]` selects one TSL or TSD item. C++ also supports this syntax for a TSB
field; Python graph code uses `getattr(bundle, symbol("field"))`. A scalar key fixes the
selection during construction; supported time series keys can retarget it.
`getattr(bundle, field)` selects a named field; the field argument is a symbol. A
missing dynamic dictionary key has no current item value. Selection follows the selected
item's updates and validity. Python object scalar overloads use their annotated result
types; see [scalar values](scalars.md#python-object-scalars).


## Construction and conversion

`convert[OUT](value)` in Python and `convert[type<OUT>](value)` in C++ copy a current
value into the requested shape. Numeric conversion is specified in
[convert](library-numeric.md#convert).

| Source | Result |
| --- | --- |
| Scalar List | Fixed TSL or TSS |
| Scalar Set | TSS |
| TSS | Scalar Set |
| Fixed TSL | Scalar List |
| Scalar Dict | TSD |
| TSD | Scalar Dict |
| Scalar Bundle | Matching named TSB |
| Named TSB | Matching scalar Bundle |

Key and item scalar types must be compatible. Keys and set members require hashing.
List-to-TSL conversion requires exactly the declared size. Bundle fields match by
name. Conversion to a scalar collection requires a valid root and valid immediate
items. The [shape-conversion example](../guide/advanced-graphs/types.md#converting-collection-shapes)
shows both languages.

`combine[OUT](...)` constructs a fixed TSL from positional time series or a TSB from
named time series. Items alias the supplied sources and retain their validity and
change information. The result type is explicit. C++ uses `combine[type<OUT>]`.


<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, TSL, graph, run_graph
from bg.operators import combine, sum_

@graph
def pair_total(left: int, right: int) -> int:
    return sum_(combine[TSL[TS[int], 2]](left, right))

print(run_graph(pair_total, [1, 2], [2, 3]))
```

Output:

```text
[3, 5]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto pair_total(wp<TS<i64>> left, wp<TS<i64>> right) {
    return sum_(combine[type<TSL<TS<i64>, 2>>](left, right));
}

int main() {
    const auto result = run_graph(pair_total, {1, 2}, {2, 3});
    std::cout << std::boolalpha << result.equals({3, 5}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

## Accumulation and emission

`collect[OUT](value, reset=...)` accumulates valid ticks into a scalar List, scalar
Set, or TSS. Dictionary results use `collect[OUT](key=key, value=value, reset=...)`
and support scalar Dict and TSD. C++ supplies `type<OUT>` and `_n` named arguments.

Only value ticks insert entries. The key input is passive; an invalid key ignores
that value tick. Invalidation preserves collected history. A valid reset tick clears
history, and a simultaneous value contributes after the reset. Reset alone publishes
a valid empty collection. Supported output guarantees are ordinary and deduplicated.

`emit(value)` publishes collection items as successive ordinary ticks. Scalar lists
and sets contribute all items of each new value. TSS contributes additions; fixed TSL
contributes modified valid items. Dictionaries contribute key/value entries, using
`KeyValue` in Python and `operators::key_value` in C++ as the result TSB schema.
TSD contributes added or modified valid entries.

The first pending event publishes immediately. Further events publish one microsecond
apart. New events append to the pending queue. Lists preserve index order; set and
dictionary order is unspecified. Empty inputs, invalidations, and removals add no
events. Invalidation preserves pending events. Stopping the owning graph releases
them.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import TS, graph, run_graph
from bg.operators import collect, emit

@graph
def collected(values: tuple[int, ...]) -> tuple[int, ...]:
    return collect[TS[tuple[int, ...]]](emit(values))

print(run_graph(collected, [(2, 4, 6)]))
```

Output:

```text
[(2,), (2, 4), (2, 4, 6)]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <bg/library.hpp>
#include <bg/node.hpp>
#include <bg/run_graph.hpp>
#include <iostream>

using namespace bg;
using namespace bg::operators;
using namespace literal;

auto collected(wp<TS<List<i64>>> values) {
    return collect[type<TS<List<i64>>>](emit(values));
}

int main() {
    const auto result = run_graph(collected, {{2, 4, 6}});
    std::cout << std::boolalpha << result.equals({{2}, {2, 4}, {2, 4, 6}}) << '\n';
}
```

Output:

```text
true
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

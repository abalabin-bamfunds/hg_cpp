# Collection operators

Use collection operators to query or summarize current items. They do not accumulate
successive input events. The code fragments use existing collection connections and
direct Python imports or C++ namespace imports.

## Collection reductions

To summarize the current items, use `sum_(values)`, `min_(values)`, `max_(values)`,
or `mean(values)` on a numeric fixed-size TSL, TSD, TSS, or scalar list, set, or
dictionary time series. They accept `i64` and `double` items. Sums and extrema retain
the item scalar type. `mean` returns `double` and accumulates in `double`; integer sums use the
[wrapping policy](library-numeric.md#arithmetic).

Scalar collection inputs are `TS<List<T>>`, `TS<Set<T>>`, or `TS<Dict<K, T>>`.
Python uses `TS[tuple[T, ...]]`, `TS[frozenset[T]]`, or `TS[dict[K, T]]`.
Dictionary reductions use values and ignore keys. Required overloads check root
validity for these inputs.

An ordinary call requires a valid collection and valid immediate items. `opt(values)`
selects an optional overload that skips invalid items:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from hgraph import opt
from hgraph.operators import sum_

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

To check a condition across a collection, use `all_(values)` or `any_(values)` on
a Boolean fixed-size TSL, TSD, TSS, or scalar list, set, or dictionary time series.
`all_` tests whether every contributing value is true; `any_` tests whether at least one is true.
They accept one collection argument. A required overload needs a valid root and valid
immediate items. `opt(values)` selects an overload that skips invalid items and treats
an invalid root as empty:

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
from hgraph import opt
from hgraph.operators import all_, any_

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

Each calculation retains no state. TSL, TSD, and scalar dictionary evaluation takes
O(N) work in the worst case; scalar lists take O(N) work with early termination. TSS
and scalar sets use one Boolean membership lookup.


## Collection queries

If a calculation depends on which keys exist, use `keys_(dictionary)` to obtain
a TSS of the dictionary's current keys. For a TSD,
it projects the existing key-set series, so value updates do not update the result.
It follows dictionary reference changes and preserves the dictionary guarantee.
For `TS<Dict<K, V>>`, it reconciles the current scalar dictionary's keys with the
output set. An unchanged key set does not publish another membership tick.

To track the keys of available items, use `keys_(valid_items(dictionary))`. The set
updates when an immediate item becomes valid or invalid. An invalid dictionary root produces
an invalid result; revalidation rebuilds the key set. `len_` and `is_empty` accept the
same valid-item view and query its current keys. These filtered queries produce
ordinary outputs.

To select keys that meet a changing Boolean condition, use `keys_where_true(mask)`.
It returns the keys whose TSD Boolean items are valid and true.
It updates when an item becomes false or invalid, or when a key is added or removed.

To pass a dictionary's current values to a list-based calculation, use
`values_(dictionary)`. For either a scalar dictionary or `TSD<K, TS<V>>`, it returns
`TS<List<V>>`, represented as `tuple[V, ...]` in Python.
The scalar list retains duplicates; its order is unspecified.
For a TSD, the list includes only valid immediate item values. A valid dictionary
with no valid items produces an empty list.

If only distinct current values matter, request `values_[TSS[V]](dictionary)` in
Python or `values_[type<TSS<V>>](dictionary)` in C++ for a TSD. The result contains
valid immediate item values, which must support equality comparison and hashing.
Removing or invalidating one item removes its value only when no other valid item
holds that value. Changes that
preserve the distinct set, including simultaneous value swaps, do not publish a
membership tick.

The TSS implementation retains a previous value per valid key and an occurrence
count per distinct value. Initialization reads all items; subsequent evaluations
process changed items without rescanning the dictionary. Root invalidation clears
this state and invalidates the result; revalidation rebuilds it.

To locate an item by value, use `index_of(values, item)` to search a scalar list or
fixed TSL. It returns the first matching index as `i64`, or `-1` if no item matches.
The item can be a scalar or a time series. TSL searches skip invalid immediate items without changing their
indices. The collection root and a time series search item must be valid.
Each search takes O(N) work and retains no state.

To count current items, use `len_(value)`, which returns an `i64` length. It counts
current TSS membership, physical TSD keys, or the items in `TS<List<T>>`, `TS<Set<T>>`,
and `TS<Dict<K, V>>`. Use
`len_(valid_items(dictionary))` to count only valid immediate TSD items. For fixed TSL
and TSB, it returns the declared item count as a wiring-time constant, independent of
item validity. These fixed-size queries add no runtime query node. Sparse TSL length is
not supplied. `is_empty(dictionary)` tests whether the dictionary has any physical keys;
`is_empty(valid_items(dictionary))` tests whether it has any valid immediate items.

To check whether an item or key is present, use `contains_(collection, key)` on a TSS,
TSD, or scalar list, set, or dictionary time series. Dictionaries test keys.
The key can be a scalar or a time series. List membership requires equality; set and dictionary lookup also requires
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
See the [scalar annotation mappings](scalars.md#built-in-mappings) for scalar lists and sets.

Find the first matching position without changing list indices:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import graph, run_graph
from hgraph.operators import index_of

@graph
def position(values: tuple[int, ...], item: int) -> int:
    return index_of(values, item)

print(run_graph(position, [(10, 20, 10), None, (30, 20)], [10, 20, 10]))
```

Output:

```text
[0, 1, -1]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto position(wp<TS<List<i64>>> values, wp<TS<i64>> item) {
    return index_of(values, item);
}

int main() {
    std::cout << run_graph(position, {{10, 20, 10}, SKIP, {30, 20}}, {10, 20, 10}) << '\n';
}
```

Output:

```text
[0, 1, -1]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

Count distinct current values, retaining a value until its last contributing key disappears:

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import INVALID, REMOVE, TS, TSD, TSS, graph, run_graph
from hgraph.operators import len_, values_

@graph
def distinct_count(items: TSD[str, TS[int]]) -> int:
    return len_(values_[TSS[int]](items))

print(run_graph(distinct_count, [
    {"a": 10, "b": 10},
    {"b": 20},
    {"a": 20},
    {"b": INVALID},
    {"a": REMOVE},
]))
```

Output:

```text
[1, 2, 1, None, 0]
```
:::::

:::::{tab-item} C++
:sync: cpp

::::{tab-set}

:::{tab-item} Code

```cpp
#include <hg/library.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;
using namespace hg::literals;

auto distinct_count(wp<TSD<string, TS<i64>>> items) {
    return len_(values_[type<TSS<i64>>](items));
}

int main() {
    std::cout << run_graph(distinct_count, {
        {{"a", 10}, {"b", 10}},
        {{"b", 20}},
        {{"a", 20}},
        {{"b", INVALID}},
        {{"a", REMOVE}},
    }) << '\n';
}
```

Output:

```text
[1, 2, 1, None, 0]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

## Scalar set operations

Two `TS<Set<K>>` inputs support union (`bit_or`, `|`), intersection (`bit_and`, `&`),
difference (`sub`, `-`), and symmetric difference (`bit_xor`, `^`). Both inputs must
have the same hashable item type. Python spells the scalar type `frozenset[K]`.
The result is a complete scalar set on each permitted evaluation.

Inputs are required by default. An invalid required input invalidates the result.
`opt()` selects overloads that treat invalid inputs as empty sets; marking either
operand selects that behavior for both operands. These overloads evaluate on input
changes without startup scheduling. Required overloads propagate the weakest observed
input guarantee; optional overloads produce ordinary outputs.

Ordinary outputs publish equal results on separate evaluations. Deduplicated outputs
suppress equal sets. Set operations retain no state between evaluations and construct
the result from the current input values.

For left and right input sizes N and M, with constant-time hash lookups, union and
symmetric difference take O(N + M) work, intersection takes O(min(N, M)), and
difference takes O(N). Temporary borrowed item views use storage bounded by the same
input counts, in addition to the result storage.

## Scalar list operators

For `TS<List<T>>` values, use `+` to concatenate two lists or append one item,
`*` with an `i64` count to repeat a list, and `-` with an item to remove every
equal item. Python uses `TS[tuple[T, ...]]` for the list value. List items may
be nested collections; subtraction requires equality-comparable items. The
list is the left operand. Item and count operands may be scalars or time
series. C++ also accepts a scalar `List<T>` on the right of concatenation.
Python scalar tuples do not currently resolve as `List<T>` operator operands;
pass both concatenated lists as `TS[tuple[T, ...]]` values.

Required inputs invalidate the result when any input is invalid. Revalidation
calculates from the current values. `opt()` on either operand selects an
optional overload and makes both operands optional. An invalid list contributes
no items to concatenation or repetition. An invalid appended or removed item
leaves the list unchanged. An invalid optional repeat count produces no tick and
preserves the current output. Before the first result, the output remains
invalid. A valid nonpositive count produces an empty list.

Required overloads propagate the weakest observed input guarantee. Optional
overloads produce ordinary outputs. Ordinary outputs publish equal results on
separate evaluations; deduplicated outputs suppress equal lists. These
implementations keep no auxiliary state. Concatenation takes O(N + M) work for
input lengths N and M. Append and removal take O(N). Repetition takes O(N × R)
work for N items and a positive repeat count R. The output requires O(N + M)
storage for concatenation, O(N) for append and removal, and O(N × R) for
repetition. An empty source list returns immediately for any count.

::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
combined = lhs + rhs
with_item = values + item
repeated = values * count
without_item = values - item
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
auto combined = lhs + rhs;
auto with_item = values + item;
auto repeated = values * count;
auto without_item = values - item;
```
:::
::::

## Scalar dictionary subtraction

`TS<Dict<K, L>> - TS<Dict<K, R>>` keeps each left key-value entry whose key is absent
from the right dictionary. The key types must match and be hashable. The value types
may differ; right values are ignored. Python uses `TS[dict[K, L]]` and
`TS[dict[K, R]]`.

Inputs are required by default. An invalid required input invalidates the result.
`opt()` on either operand selects the optional overload: an invalid left dictionary
produces an empty result, and an invalid right dictionary leaves the left dictionary
unchanged. Optional outputs are ordinary. Required outputs propagate the weakest
observed input guarantee. Ordinary outputs publish equal results on separate
evaluations; deduplicated outputs suppress equal dictionaries.

The operator retains no state. For N left entries, it takes expected O(N) work and
O(N) temporary storage for borrowed entries, in addition to the result storage.

## Time series set operations

To combine changing memberships, use union (`bit_or`, `|`), intersection (`bit_and`, `&`),
difference (`sub`, `-`), or symmetric difference (`bit_xor`, `^`) on two `TSS<K>` inputs. Both inputs must
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
from hgraph import opt
from hgraph.operators import is_empty

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

If a consumer needs a different key or grouping, use key-routing transforms to
reorganize a dictionary without copying its item values.
Their reference outputs identify source items. Target updates reach downstream
consumers without evaluating the transform again.

Choose the transform according to how the consumer identifies items. In this table,
`V` denotes a time series type:

| Need | Operator | Input | Output |
| --- | --- | --- | --- |
| Address items by another key | `rekey(ts, new_keys)` | `TSD<K, V>`, `TSD<K, TS<K1>>` | `TSD<K1, REF<V>>` |
| Group items by a changing category | `partition(ts, partitions)` | `TSD<K, V>`, `TSD<K, TS<P>>` | `TSD<P, TSD<K, REF<V>>>` |
| Combine groups into one dictionary | `unpartition(ts)` | `TSD<P, TSD<K, V>>` | `TSD<K, REF<V>>` |
| Exchange outer and inner key levels | `flip_keys(ts)` | `TSD<K, TSD<K1, V>>` | `TSD<K1, TSD<K, REF<V>>>` |
| Address nested items by key pairs | `collapse_keys(ts)` | `TSD<K, TSD<K1, V>>` | `TSD<Tuple<K, K1>, REF<V>>` |
| Split key pairs into nested groups | `uncollapse_keys(ts)` | `TSD<Tuple<K, K1>, V>` | `TSD<K, TSD<K1, REF<V>>>` |

Existing reference items retain their targets without adding another reference
layer. Invalid item targets retain their keys. An absent or invalid mapping item
in `rekey` or `partition` removes that source item's route. Source and mapping
changes both update routes. An input becoming invalid through reference rebinding
removes its routes. A valid empty source establishes a valid empty result.

`partition`, `flip_keys`, and `uncollapse_keys` remove empty output groups by
default. Pass `remove_empty=false` to retain groups after their last item is
removed. Python spells the value `False`.

To look up a source key by its value, use `flip(ts, unique=true)` to reverse a
`TSD<K, TS<V>>` into `TSD<V, TS<K>>`.
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

To follow one item from a larger collection, use `collection[key]` to select a
TSL or TSD item. C++ also supports this syntax for a TSB
field; Python graph code uses `getattr(bundle, symbol("field"))`. A scalar key fixes the
selection during construction; supported time series keys can retarget it.
`getattr(bundle, field)` selects a named field; the field argument is a symbol. A
missing dynamic dictionary key has no current item value. Selection follows the selected
item's updates and validity. Python object scalar overloads use their annotated result
types; see [scalar values](scalars.md#python-object-scalars).


## Construction and conversion

If two graph components use different collection shapes, use `convert[OUT](value)`
in Python or `convert[type<OUT>](value)` in C++ to copy the current value into the
required shape. Numeric conversion is specified in
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

For scalar Dict to TSD conversion, request `deduplicated` on the TSD or its
items to suppress unchanged output updates without deduplicating the input Dict.
An omitted output guarantee retains the input guarantee.

To present several existing connections as one collection, use `combine[OUT](...)`.
It constructs a fixed TSL from positional time series or a TSB from named time series.
Items alias the supplied sources and retain their validity and change information.
The result type is explicit; C++ uses `combine[type<OUT>]`.


<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from hgraph import TS, TSL, graph, run_graph
from hgraph.operators import combine, sum_

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto pair_total(wp<TS<i64>> left, wp<TS<i64>> right) {
    return sum_(combine[type<TSL<TS<i64>, 2>>](left, right));
}

int main() {
    const auto result = run_graph(pair_total, {1, 2}, {2, 3});
    std::cout << result << '\n';
}
```

Output:

```text
[3, 5]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

## Accumulation and emission

To build a collection from successive events, use `collect[OUT](value, reset=...)`. It accumulates
valid ticks into a scalar List, scalar Set, or TSS. Dictionary results use `collect[OUT](key=key,
value=value, reset=...)` and support scalar Dict and TSD. C++ supplies `type<OUT>` and `_n` named
arguments.

Only value ticks insert entries. The key input is passive; an invalid key ignores
that value tick. Invalidation preserves collected history. A valid reset tick clears
history, and a simultaneous value contributes after the reset. Reset alone publishes
a valid empty collection. Supported output guarantees are ordinary and deduplicated.

If a downstream calculation processes one item at a time, use `emit(value)` to publish
collection items as successive ordinary ticks. Scalar lists and sets contribute all
items of each new value. TSS contributes additions; fixed TSL
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
from hgraph import TS, graph, run_graph
from hgraph.operators import collect, emit

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
#include <hg/library.hpp>
#include <hg/node.hpp>
#include <hg/run_graph.hpp>
#include <iostream>

using namespace hg;
using namespace hg::operators;
using namespace literal;

auto collected(wp<TS<List<i64>>> values) {
    return collect[type<TS<List<i64>>>](emit(values));
}

int main() {
    const auto result = run_graph(collected, {{2, 4, 6}});
    std::cout << result << '\n';
}
```

Output:

```text
[[2], [2, 4], [2, 4, 6]]
```
:::

:::{tab-item} Build and run

The [C++ example project](../guide/installation.md#c-example-project) builds this code as `main.cpp`.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH="/path/to/site-packages"
cmake --build build --config Release
```

Run `./build/example` on Linux or macOS, or `build/Release/example.exe` on Windows.
:::
::::
:::::
::::::

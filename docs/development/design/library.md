# Standard library

## Public surface

C++ graph code includes `<bg/library.hpp>` and links `bg::library`. Python
graph code uses `bg.operators`. Both surfaces expose native library operators
and the engine's `const_`, `default_`, `getitem`, `getattr`, `map`, `mesh`, and
`switch_` operators. All use normal overload resolution.

```cpp
#include <bg/library.hpp>

using namespace bg;

const auto calculate = graph([](wp<TS<double>> x, wp<TS<double>> y) {
    return (x + 2.0) * y;
});
```

```python
import bg

@bg.graph
def calculate(x: bg.TS[float], y: bg.TS[float]) -> bg.TS[float]:
    return (x + 2.0) * y
```

## Scalar operators

Built-in operands can be scalars or time series. Numeric operands have exact
types; an integer operand does not convert to double to match an overload.

| Operators | Supported scalar types |
| --- | --- |
| `add` | `i64`, `double`, `timedelta_t`, `string`; timestamp plus duration in either order |
| `sub` | `i64`, `double`, `timedelta_t`; timestamp minus duration or timestamp |
| `mul` | `i64`, `double`; string or symbol repeated by an `i64` in either order |
| `div` | `i64`, `double`; result is `double` |
| `floordiv` | `i64` to `i64`, `double` to `double` |
| `mod` | `i64` to `i64`, `double` to `double` |
| `pow` | `i64` to `i64`, `double` to `double` |
| `round` | `double`, with an optional decimal digit count; result is `double` |
| `neg`, `pos`, `abs` | `i64`, `double`, `timedelta_t` |
| `eq`, `ne` | Compatible values of an equality-comparable scalar type |
| `lt`, `le`, `gt`, `ge` | `i64`, `double`, `string`, `date_time_t`, `timedelta_t` |
| `and_`, `or_`, `not_` | `bool` |

Generic `eq` and `ne` overloads accept two time series of the same
equality-comparable scalar type. Scalar-only and mixed scalar/time-series
calls for compound, collection, and extension types are not yet supplied.

Timestamp subtraction produces a duration. Timestamp minus duration produces a
timestamp. Integer and temporal arithmetic wraps at the underlying integer
width. Negation and absolute value of the minimum integer or duration return
that same minimum value. Floating-point arithmetic uses normal C++ operations.
String concatenation, text repetition, string comparison, and substring search process
UTF-8 bytes, including embedded NUL bytes. Character operations follow the
[text rules](#text-operations). Temporal operators are native C++ overloads.
Python `datetime.timedelta` maps to
`timedelta_t`; Python timestamp scalar conversion is not yet supplied.

Integer division converts both operands to `double` and uses native floating-point
division. Conversion can round operands outside the consecutive-integer range,
so large-integer results can differ from Python integer true division. It preserves
the sign of a zero result. Division, floor division, and modulo reject a zero divisor
during evaluation.
Dividing the `i64` minimum by `-1` returns the positive quotient as `double`;
integer floor division wraps the corresponding quotient, and modulo returns
zero. Integer power wraps at `i64` width and rejects negative exponents.
Floating power rejects zero raised to a negative exponent and negative bases
with fractional exponents. `round` returns `double` and rounds ties to even.
With a digit count, it scales the value by a power of ten, rounds, and scales
back. Floating-point scaling can affect the result: `round(2.675, 2)` returns
`2.68`. It preserves non-finite values and the sign of zero.

`convert` requires an explicit full result time-series type:

```cpp
operators::convert[type<TS<double>>](integer_value);
```

The built-in overloads convert between `i64` and `double`. Converting `double`
to `i64` truncates toward zero; NaN raises `domain_error`, and infinities or
values outside the `i64` range raise `overflow_error`. The public operator
accepts overloads for other source time-series types, including collections;
each overload's output must be compatible with the requested result type. The
requested type checks compatibility and does not replace the overload's
inferred output guarantee. See [collection conversion](#collection-conversion-and-construction)
for the collection overloads.

Required operands gate evaluation. Their invalidation invalidates the result,
and revalidation permits another calculation. Equal results from ordinary
inputs still produce ticks. Required calculations propagate the weakest
observed input guarantee; output publication enforces it. Reference inputs
follow the [reference guarantee rules](type-system.md#reference-guarantees).

Scalar-only calls, except string concatenation, use the same node implementations
and evaluate once in the first cycle. Required calculations infer a `const`
output. Compilation retains the calculation node. Scalar-only string
concatenation currently calculates during wiring and returns `const_`.

## Optional arithmetic

`opt()` selects optional overloads. Addition substitutes zero, or the empty
string, for invalid operands. Multiplication substitutes one. These overloads
accept two optional operands and produce ordinary outputs. Marking either
operand optional selects that behavior for both operands.

```cpp
auto total = opt(x) + opt(y);
auto product = opt(x) * opt(y);
auto with_fallback = operators::default_(x, 10.0) + y;
```

Time series calls do not schedule on start. An input change can calculate a
result while either operand is invalid. No input change means no calculation.
`default_()` supplies a caller-selected fallback; it does not select optional
arithmetic. Other arithmetic operators reject `opt()` arguments.

## Collection conversion and construction

`convert[OUT](value)` copies the current value into the requested time series
shape. C++ uses `convert[type<OUT>](value)`; Python uses `convert[OUT](value)`.
The collection overloads support these conversions:

| Source | Result |
| --- | --- |
| `TS<List<V>>` | Fixed `TSL<TS<V>, N>` or `TSS<V>` |
| `TS<Set<K>>` | `TSS<K>` |
| `TSS<K>` | `TS<Set<K>>` |
| Fixed `TSL<TS<V>, N>` | `TS<List<V>>` |
| `TS<Dict<K, V>>` | `TSD<K, TS<V>>` |
| `TSD<K, TS<V>>` | `TS<Dict<K, V>>` |
| `TS<Bundle<SCHEMA>>` | Named TSB with matching scalar-valued fields |
| Named TSB with scalar-valued fields | `TS<Bundle<SCHEMA>>` |

Python uses `tuple[T, ...]` for scalar List annotations. Scalar Set annotations
are not yet exposed; `bg.TSS[T]` is available. See the
[Python type mappings](python.md).

Set items and dictionary keys must support hashing. List-to-TSL conversion
requires exactly `N` items and reports a size mismatch during evaluation.
Bundle fields match by name and use directional scalar compatibility. An
absent optional scalar field invalidates its destination TSB item.
Bundle-to-TSB conversion resolves field indices during wiring.

Conversions to scalar collections or a scalar Bundle require a valid root and
valid immediate time series items. Other conversions require a valid source
value. Each conversion propagates the weakest observed input guarantee. The
requested result must accept that guarantee. Publication and invalidation use
the destination's normal mutation rules.

`combine[OUT](...)` constructs a free fixed TSL from positional time series
arguments or a free TSB from named time series arguments during wiring.
It requires an explicit result type with compatible items:

```cpp
auto pair = operators::combine[type<TSL<TS<i64>, 2>>](x, y);
```

For an empty TSL, pass a typed empty pack as `"values"_n = tsl<TS<i64>>()`.

Items alias the supplied time series and retain their normal validity and
change information. `combine` adds no copying node or owned item values.
The requested guarantees must be compatible with the free collection.
Validity requirements are opt-in: a consuming node declares `all_valid` when
it needs every immediate item to be valid. Collection materialization follows
the normal [free-collection wiring rules](graph-wiring.md).

## Collection accumulation and emission

`collect[OUT](value, reset=...)` accumulates valid value ticks into a scalar
List, scalar Set, or TSS. Dictionary results use
`collect[OUT](key=key, value=value, reset=...)` and support scalar Dict and TSD
outputs. The key input is passive: only a value tick inserts or replaces an
entry, and an invalid key ignores that value tick.

Collect requires an explicit result type. It supports ordinary and
deduplicated guarantees, including TSD item guarantees. It does not schedule
on start. Source invalidation preserves history. Any valid reset tick clears
history, regardless of its payload; a simultaneous valid value contributes
after the reset. Reset alone produces a valid empty collection.

Scalar collection results publish complete snapshots. Ordinary scalar Set and
Dict results publish even when insertion leaves the value unchanged;
deduplicated results suppress equal snapshots. TSS and TSD results publish
their normal membership and item changes.

`emit(value)` publishes collection items as successive ordinary ticks:

| Source | Events |
| --- | --- |
| `TS<List<V>>`, `TS<Set<V>>` | Every item in each new scalar value |
| `TS<Dict<K, V>>` | Every key-value entry in each new scalar value |
| `TSS<V>` | Added items |
| Fixed `TSL<TS<V>, N>` | Modified valid items |
| `TSD<K, TS<V>>` | Added or modified valid entries |

Dictionary events use the public `operators::key_value` schema, exposed as
`bg.KeyValue` in Python. Its `key` and `value` field types resolve from the
source dictionary. A downstream node can name the schema directly:

```cpp
const auto read_value = node(
    [](In<TSB<operators::key_value>> entry, Out<TS<operators::KEY_VALUE_VALUE>> output) {
        output.copy(entry->value);
    });
```

```python
@bg.compute_node
def read_value(entry: bg.TSB[bg.KeyValue]) -> bg.TS[int]:
    return entry.value.value
```

Both declarations use the native schema's identity. A separately declared
schema with the same field names is a different named type.

Other events are `TS<V>`. Emit publishes the first pending event immediately
and schedules remaining events one microsecond apart. New events append behind
pending events. Lists use index order; set and dictionary enumeration order is
unspecified.

Emit retains owning scalar snapshots until publication. Empty inputs,
removals, and invalidations contribute no events; input invalidation preserves
the output and pending events. Stopping the owning graph releases the queue
and its retained values. Emit infers its result type; an explicit result, when
supplied, must be compatible and have ordinary guarantees.

## Validity and tick selection

`valid(value)` reports root validity. `all_valid(value)` reports whether the
root and every immediate contained time series are valid, following the
[validity rules](time-series-validity.md#nodes). Both evaluate in the first
cycle, accept initially invalid inputs, and produce deduplicated Boolean
outputs. Reference arguments observe their targets through normal input
bindings.

`sample(signal, value)` copies the complete current value when a valid signal
ticks and `value` is valid. Changes to `value` alone do not schedule evaluation.
Invalidating either input preserves the last sampled output. The output has
ordinary guarantees, including its contained time series, so a later signal
can sample an unchanged value again. Copying uses the normal collection
mutation rules: unchanged set membership and already-valid empty dictionaries
do not publish another tick. Contained references copy reference values, not
their target values. See [output copying](node-api.md#output-parameters).

`dedup(value)` forwards changes through an output with at least the
deduplicated guarantee. Equality is exact and uses the scalar type's equality
operation; floating-point values use no tolerance. Inputs already observed as
deduplicated, final, or const are returned without adding a node. Invalidation
and revalidation follow the normal output rules. Collection outputs apply the
guarantee recursively and retain stronger contained guarantees.

`filter_(condition, value)` forwards changes while the condition is valid and
true. It preserves its output while closed. Opening publishes the current value
if it has changed since the last forwarding; repeated true condition ticks do
not republish an unchanged value. Reopening reconciles the complete collection,
including removals while closed. Input invalidation propagates while open;
TSS and TSD outputs become valid empty collections rather than root-invalid.
The output retains the input's root guarantee up to `final`. Contained time
series are limited to that same guarantee because reopening can copy unchanged
items. A const input becomes final because its first forwarding can be delayed.

`if_then_else(condition, true_value, false_value)` selects an input by reference.
Changing the condition exposes the selected input's current value. Updates to
that input remain observable without copying. An invalid condition or selected
input makes the observed result invalid. Updates to the other input and
repeated identical condition values do not publish additional ticks. Selection
applies to the whole collection. Both inputs must have a
[common time series type](type-system.md#time-series-guarantees).

These operators are available under `bg::operators` in C++ and `bg.operators`
in Python.

## Merge

`merge(values...)` requires at least one input. It merges compatible time
series of the same shape. With multiple inputs, it weakens the common output
guarantee to ordinary; one input is forwarded unchanged.

For scalar time series, each tick selects the leftmost valid input that
changed. If no input changes, the result keeps its previous value. An invalid
input is skipped. The result remains invalid until a valid input first ticks.

For collections, `merge` combines matching fields or items recursively. TSS
inputs produce the union of their keys. TSD inputs produce the union of their
keys and merge the corresponding values. TSB fields merge by name. TSL inputs
merge corresponding items and must have fixed size; sparse TSL inputs are not
supported. A named `values` argument can supply a prebuilt TSL of inputs.

## Counting and one-event lag

`count(value)` publishes `1` on the first valid input tick and increments on
each later valid tick. Invalidation invalidates the output and resets the
count; revalidation starts at `1`. It counts root ticks, not collection items,
and propagates the observed input's root guarantee. Incrementing uses the
`i64` wrapping policy described above.

`lag(value)` accepts `TS<T>` and publishes the preceding input event whenever
the input changes. It retains an owning copy of the scalar value. Invalidation
is also an event and is delayed by one input event:

```text
input:   10   20   invalid   30        40
output:  —    10   20        invalid   30
```

Neither operator schedules on start. Skipped cycles do not advance either
operation. Ordinary equal-valued ticks remain separate events. `lag` produces
an ordinary output; its first event produces no output, and its final retained
event remains unpublished unless another input event arrives.

## Analytical operators

`diff(value)` accepts `TS<i64>` and `TS<double>`. It subtracts the previous
valid input value from each current value. The first value after graph start or
input invalidation has no difference. Skipped cycles do not advance the
calculation; equal-valued input ticks remain separate events. The output has
the ordinary guarantee.

`clip(value, min_, max_)` accepts `i64` or `double` values. Each argument can
be a scalar or a time series of the matching type. It clamps each result to
the inclusive bounds. Required time series arguments gate evaluation;
invalidating any required input invalidates the output. When all
inputs are valid, `min_ > max_` raises an evaluation error. Time series
arguments propagate their weakest observed guarantee.
Floating-point bounds and values use ordinary comparisons.

`ewma(value, alpha, min_periods=0)` accepts `TS<double>` and scalar
configuration. `alpha` must be finite and in `[0, 1]`; `min_periods` must be
nonnegative. The first valid value seeds the average. Each later valid value
updates it as `average += alpha * (value - average)`. The operator suppresses
the first `min_periods` valid ticks and publishes on the next valid tick.
Invalidation invalidates the output and clears the average and warmup state;
revalidation starts a new average and repeats the warmup threshold. Skipped
cycles do not advance the state.
With zero warmup, EWMA retains the input guarantee. Positive warmup limits the
output guarantee for a `const` input to `final` because its only tick may be
suppressed. Other input guarantees are retained.
## Counted tick selection

`take(value, n)` forwards the first `n` valid input ticks, then preserves its
output. `drop(value, n)` skips the first `n` valid input ticks and forwards
subsequent ticks. `step(value, n)` forwards the first valid input tick and then
every `n`th valid tick: positions `0`, `n`, `2n`, and so on.

These operators count valid root ticks, not collection items. Equal ordinary
ticks count separately. Skipped cycles and root invalidation neither advance
nor reset the count. Revalidation resumes the existing count.

`take` propagates invalidation until its count is complete; later invalidation
preserves its output. `drop` propagates invalidation after forwarding begins.
`step` propagates invalidation independently of valid-tick selection. For TSS
and TSD, propagation follows the normal [copying rules](node-api.md#output-parameters)
and clears an existing output to a valid empty collection.

The first forwarded collection value includes changes made while ticks were
skipped. Each selected `step` tick copies the complete current collection,
including removals, rather than only the latest delta.
`take` forwards deltas. `drop` copies the complete value when forwarding begins
and forwards deltas afterward.

Counts are scalar arguments. `take` and `drop` require `n >= 0`; `take(value, 0)`
publishes nothing, and `drop(value, 0)` forwards every tick. `step` requires
`n > 0`; `step(value, 1)` forwards every tick. Invalid counts fail during wiring.
None of these operators schedules on start.

`take` retains the observed input type and its guarantees, except that a zero
count limits const guarantees to final. `drop` and `step` retain the observed
root guarantee up to final and limit contained guarantees to that same level.
 This permits complete-value copies of unchanged items after skipped ticks.

## Timed operators

`delay(value, duration)` publishes each input event at its original evaluation
time plus `duration`. It retains ticks, including invalidation and collection
deltas, rather than sampling the value later. Equal ordinary ticks remain
separate events. Zero duration returns the input; negative duration is a wiring
error. Positive delay limits const guarantees to final, including contained
time series. Contained references are dereferenced before ticks reach delay, so
it retains target values and collection deltas rather than reference bindings.
If a referenced TSS or TSD target becomes invalid, its removal delta is retained
and replayed for direct TSS/TSD values and TSD-contained references. A fixed
TSL containing a reference to a TSS does not currently propagate the target's
invalidating removal through the list. Delay rejects sparse TSL values.
Retained ticks follow the supported types and ownership rules in
[retaining ticks](time-series-types.md#retaining-ticks). Initially unbound
references are supported.

`schedule(period, initial_delay=true, max_ticks=...)` publishes ordinary
`TS<bool>` ticks whose value is true. The first tick occurs one period after
graph start. `initial_delay=false` produces the first tick at graph start.
`max_ticks` limits the number of ticks; zero produces none. Without a limit,
the source continues until its graph stops. Period must be positive and
`max_ticks` must be nonnegative.

`throttle(value, period, delay_first_tick=false)` passes the first event
immediately and starts a period. Events during that period coalesce. At its
end, throttle publishes the latest pending state and starts another period.
With no pending event, it stops scheduling until the input changes again.
`delay_first_tick=true` also delays the first event by one period. Period must
be positive.

Throttle applies the same timing to invalidation. An input change at the
publication time contributes to that publication. Collection publication
reconciles the complete current value, so changes that cancel within the
period need not produce an output tick. The output retains input guarantees up
to final. Throttle retains timing state, not a copy of the input value.

`resample(value, period)` composes `schedule(period)` and `sample`. Its first
sampling time is one period after graph start. Each scheduled tick samples the
complete current valid value; input changes alone do not publish. Invalid input
preserves the previous output, following `sample` above. Collection mutation
rules still apply, so an unchanged collection need not publish another root tick.

Durations are scalar arguments: use `bg::timedelta_t` in C++ and
`datetime.timedelta` in Python. Timed work uses evaluation time and stops with
the owning graph, including inner graphs. Scheduling outside the supported
[evaluation-time range](evaluation.md#evaluation-time) fails.

## Running calculations

`sums(value)`, `mins(value)`, `maxs(value)`, and `means(value)` calculate over
successive ticks of one `TS<i64>` or `TS<double>`. `sums`, `mins`, and `maxs`
retain the input scalar type. `means` returns `double` and accumulates in
`double`, independently of the integer sum's wrapping policy.

Each valid input tick contributes, including equal ordinary ticks. Skipped
cycles do not advance the calculation. Invalidation clears history and
invalidates the output; revalidation starts a new history. No operator
schedules on start. `mins` and `maxs` publish the first value and then publish
only when a comparison establishes a new extremum.

`mins` and `maxs` use ordinary floating-point comparisons. An initial NaN
remains the result until history is cleared; a later NaN produces no tick.
NaN is a valid scalar value, not time series invalidity.

Without reset, `sums` and `means` propagate the observed input guarantee.
`mins` and `maxs` provide at least `deduplicated` and retain `final` or `const`
inputs. With reset, `sums` and `means` produce ordinary outputs, while `mins`
and `maxs` produce deduplicated outputs. Reset can invalidate these outputs
independently of the value input's guarantee.

Each operator accepts an optional `reset` time series argument. Any valid tick
on that input clears history, regardless of its value. A simultaneous valid
`value` tick starts the new history. Reset alone invalidates the output rather
than sampling an unchanged input. Invalidating `reset` does not clear history
or contribute another value.

```cpp
auto total = operators::sums(value);
auto restarted_total = operators::sums(value, "reset"_n = reset);
```

```python
total = bg.operators.sums(value)
restarted_total = bg.operators.sums(value, reset=reset)
```

## Collection reductions

`sum_(values)`, `min_(values)`, `max_(values)`, and `mean(values)` calculate
over the current items of a numeric fixed-size TSL, TSD, or TSS. They accept `i64` and
`double` items. Sums and extrema retain the item scalar type. `mean` returns
`double` and accumulates in `double`; integer sums use the wrapping policy
described above.

A required overload needs a valid collection and valid immediate items.
`opt(values)` selects an optional overload that skips invalid items:

```cpp
auto complete_total = operators::sum_(values);
auto available_total = operators::sum_(opt(values));
```

```python
complete_total = bg.operators.sum_(values)
available_total = bg.operators.sum_(bg.opt(values))
```

With no contributing items, sum publishes zero and the other reductions
invalidate their output. An invalid optional collection has no contributing
items. These nodes do not schedule on start; an input change schedules the
calculation. Skipped cycles preserve the previous result.

Each evaluation reads the complete current collection. Item updates,
invalidations, additions, removals, and reference changes therefore contribute
to the next result without retaining previous item values. Ordinary outputs
publish on each permitted evaluation, including equal results. Extrema use
ordinary floating-point comparisons in enumeration order. TSD and TSS
enumeration order is unspecified.

The `all_valid` sum propagates the observed collection guarantee. Optional sum
always has an ordinary output and publishes on each evaluation, including
equal results. For example, binding an invalid set and then observing `{0}`
publishes `0` twice.

The other reductions retain ordinary, deduplicated, and final guarantees, and
limit const to final because an empty collection has no result. This applies
to both overload versions. Each evaluation takes O(N) work and O(1) auxiliary
storage.

## Boolean collection reductions

`all_(values)` and `any_(values)` reduce a Boolean fixed-size TSL, TSD, or TSS.
`all_` tests whether every contributing value is true; `any_` tests whether at
least one is true. They accept one collection argument. A required overload
needs a valid root and valid immediate items. `opt(values)` selects an
overload that skips invalid items and treats an invalid root as empty:

```cpp
auto complete = operators::all_(values);
auto available = operators::any_(opt(values));
```

```python
complete = bg.operators.all_(values)
available = bg.operators.any_(bg.opt(values))
```

Use `valid_items(values)` to exclude invalid TSD items while still requiring
root validity, following the [valid-item rules](operators.md#valid-item-arguments).
It can also be combined with `opt()`.

With no contributing items, `all_` publishes true and `any_` publishes false.
Input changes recalculate the result, including item validity changes and
removals. Neither operator schedules on start. Ordinary outputs publish equal
results on separate evaluations. Required overloads propagate the observed
collection guarantee; optional overloads produce ordinary outputs.

Each calculation retains no state. TSL and TSD evaluation takes O(N) work in
the worst case; TSS uses one Boolean membership lookup.

## Collection queries

`len_(value)` returns an `i64` length. It counts current TSS membership or the
items in `TS<List<T>>`, `TS<Set<T>>`, and `TS<Dict<K, V>>`. For fixed TSL and TSB,
it returns the declared item count as a wiring-time constant, independent of
item validity. These fixed-size queries add no runtime query node. Sparse TSL
length is not supplied.

`contains_(collection, key)` tests current membership in TSS, TSD, or a scalar
list, set, or dictionary time series. Dictionaries test keys. The key can be a
scalar or a time series. List membership requires equality; set and dictionary
lookup also requires hashing.

TSS and TSD queries subscribe to the selected key. Unrelated key changes do not
schedule the query. Ordinary TSD membership ignores item validity and value
updates. Use `contains_(valid_items(collection), key)` to require a valid
immediate item, following the [valid-item rules](time-series-validity.md#collections).
A valid collection with no matching key produces `false`; an invalid collection
has no membership result. A time series key tick recomputes membership,
including an equal-valued ordinary tick.

String and symbol overloads count Unicode code points with `len_` and test
substrings with `contains_`, including embedded NUL bytes. Both substring
operands have the same scalar type. These overloads accept scalars and time
series.

Runtime queries require valid time series inputs and propagate their weakest
observed guarantee. An initially invalid collection does not produce a result.
Ordinary inputs can publish equal query results on successive changes;
deduplicated outputs suppress equal results.

Use named calls in Python. Its `len()` and `in` syntax cannot return wiring
points. Annotate an engine scalar list as `tuple[T, ...]`; scalar set
annotations are not yet supplied.

## Collection selection

`keys_where_true(mask)` returns a TSS containing the keys whose current TSD
Boolean items are valid and true. False or invalid items are absent. The output
follows item additions, updates, removals, and reference target validity changes.
It remains invalid until the mask is valid; an invalid mask clears an existing
output.

TSD `getitem` accepts a TSS of keys. It returns a dictionary of references for
the selected keys. A selected key absent from the source dictionary remains in
the result with an invalid reference. This composes with `keys_where_true`:
C++ uses `values[operators::keys_where_true(mask)]`; Python uses
`values[bg.operators.keys_where_true(mask)]`.

## Text operations

Character operations interpret strings and symbols as UTF-8. `len_`, indexing,
and `substr` count Unicode code points, not bytes or grapheme clusters.
Combining marks count separately. These operations and case conversion reject
malformed UTF-8; string storage itself does not validate or normalize text.

`substr(text, start, end)` selects the half-open range `[start, end)`.
Omitting `end` selects through the end of the text. Negative positions count
from the end; slice bounds clamp to the available code points. Reversed bounds
produce an empty string. `text[index]` returns one code point as a string and
raises an error for an out-of-range index.

`lower(text)`, `upper(text)`, and `casefold(text)` use Unicode case mappings
independent of the process locale. Case conversion can change the number of
code points. `casefold` provides caseless comparison text; it does not perform
Unicode normalization. ICU limits case-conversion input to 2,147,483,647 bytes.

Multiplying text by an integer repeats it. Zero and negative counts produce
an empty string. Oversized results fail before allocating the result. Text-producing
operations on symbols return strings rather than interned symbols.

These operations accept scalar or time-series arguments, require valid time
series inputs, and propagate the weakest observed input guarantee. Invalidating
a required input invalidates the result. Ordinary inputs can publish equal
results on separate ticks. Unicode operations use ICU, linked by `bg::library`.

`str_(value)` returns Python-style text for built-in scalar values and nested
scalar collections. It rejects unsupported scalar extension types.
`format_(pattern, *values, **named)` uses `{fmt}` format strings and preserves
argument types for format specifications. It evaluates scalar-only calls at
graph start. The pattern and every formatting argument must be valid.
`print_(pattern, *values, **named)` writes the formatted text and a
newline to standard output. Set `__std_err__=True` to write to standard error;
`__std_err__` must be a scalar Boolean. Print nodes are not deduplicated during
wiring.

`print_(series)` writes the current tick. Formatting arguments to `print_` also
accept collection time series. Collection arguments use the
[tick formatter](time-series-types.md#tick-formatting) to print changes, including removals and
invalidation. Valid scalar arguments retain native `{fmt}` format specifications.
Tick views, including `SKIP` and `INVALID`, accept only an empty specification.
Unchanged collection arguments print `SKIP`; valid scalar arguments use their
current values.
A lone string time series is tick data; a scalar string is a format pattern.
A string-series pattern requires at least one formatting argument.

`log_(series)` and `log_(pattern, *values, **named)` use the same argument and
tick formatting rules as `print_`. The scalar string option `__level__` selects
severity and defaults to `"info"`. Logging nodes are not deduplicated.

Logging uses the process-level Quill logger named `bg`. The node checks its
enabled level before capturing data. Enabled messages own their pattern,
argument names, scalar values, and serialized ticks. The logging thread builds
the format arguments and calls the existing scalar and
[serialized tick formatters](time-series-types.md#tick-formatting).
Captured ticks use the existing serializer representation and reader cleanup.
This is required to release encoded scalar payloads even when formatting fails.
Changed invalid inputs retain an invalid tick header so collection invalidation
uses the same formatting rules as `print_`.
Extension type descriptors and registered callbacks must remain available until
queued messages have been released.

`<bg/logging.hpp>` exposes `bg::logging::initialize(sinks, pattern_options,
backend_options)` for application startup. It uses Quill's configuration types
and rejects initialization after BG's logger exists or Quill's backend starts.
Initialization paths are serialized; subsequent `logger()` calls return a cached
pointer. Empty sinks select stdout. Lazy initialization retains the same default
and adopts an existing application-managed `bg` logger. The default pattern
includes time, level, logger name, message, and MDC fields. It omits thread ID
and source location and formats local wall time with microsecond precision.
Explicit pattern options replace these defaults.
`bg::logging::logger()` exposes runtime level changes through Quill;
`bg::logging::flush()` waits for queued messages.

### Regular expressions

`matches(pattern, text)` returns whether the pattern occurs anywhere in the
text. Use anchors to constrain the match position. It returns a Boolean without
extracting or storing capture strings.

`match_(pattern, text)` returns a fixed TSL of string captures. Group 1 is at
index 0; the complete match is not included. The scalar pattern determines the
TSL size during wiring. A pattern without capture groups is a wiring error;
use `matches` instead. Unmatched groups are invalid, while a matched empty
group contains a valid empty string. Losing the match invalidates every item.
If all groups are absent, the TSL is invalid under the normal
[TSL validity rule](time-series-validity.md#collections), even when the search
succeeds. Capture output guarantees limit const to final.

```python
found = bg.operators.matches(r"(\w+)-(\d+)", text)
captures = bg.operators.match_(r"(\w+)-(\d+)", text)
word, number = captures[0], captures[1]
```

`replace(pattern, replacement, text)` replaces every non-overlapping match.
Patterns use ICU syntax, including `(?<name>...)` for named groups.
Replacement templates use Python-style references:
`\1`, `\g<name>`, and `\g<0>` for the complete match. An unmatched group
contributes an empty string. `$` is literal. Invalid patterns and replacement
templates raise errors during wiring.

Pattern and replacement arguments are scalars. Text accepts a string or symbol,
supplied as a scalar or time series. Wiring prepares the immutable compiled
pattern and replacement template. The compiled graph owns them and shares them
across its runtime instances. Each runtime node creates a separate mutable
matcher at startup and reuses it across ticks. All three operators reject
malformed UTF-8 and use required inputs. `matches` and `replace` propagate the
observed text guarantee.

### Split and join

`split(text, separator)` returns `TS<List<string>>`. A supplied `maxsplit`
limits the number of separators consumed; a negative limit means unlimited.
An empty separator raises an error. Leading, trailing, and adjacent separators
produce empty pieces. Split and join preserve bytes without normalization.

An explicit `TSL<TS<string>, N>` result requires `N > 0`. Split consumes at
most `N - 1` separators and retains the remaining text in the final item.
A smaller explicit `maxsplit` still limits consumption. Producing fewer than
`N` pieces raises an error.

Select the result type through the operator's explicit type argument:

```cpp
auto pieces = operators::split[type<TSL<TS<string>, 3>>](text, ",");
```

```python
pieces = split[TSL[TS[str], 3]](text, ",")
```

`join(values, separator)` joins a scalar list or fixed time series list in item
order. The default TSL overload requires the root and all immediate items
to be valid. `join(opt(values), separator)` skips invalid items and treats
an invalid root as empty. Optional join publishes an ordinary output on
input changes; it does not schedule an initial evaluation. Empty input
produces an empty string. Both strings and symbols produce string output.

## Time series set operations

Two `TSS<K>` inputs support union (`bit_or`, `|`), intersection (`bit_and`,
`&`), difference (`sub`, `-`), and symmetric difference (`bit_xor`, `^`).
Both inputs must have the same key type. `eq` and `ne` compare membership;
`is_empty` tests whether a set has no members.

Inputs are required by default. A TSS result preserves its membership while
a required input is invalid and reconciles the current sets when evaluation
resumes. Boolean results follow normal required-input invalidation.

`opt()` selects overloads that treat invalid inputs as empty sets. Marking
either binary operand selects that behavior for both operands. Optional
overloads evaluate on input changes, without startup scheduling.

```cpp
auto shared = lhs & rhs;
auto available = opt(lhs) | opt(rhs);
auto empty = operators::is_empty(available);
```

```python
shared = lhs & rhs
available = bg.opt(lhs) | bg.opt(rhs)
empty = bg.operators.is_empty(available)
```

Set results publish only actual membership changes, including a first valid
empty result. Updating one operand without changing the result membership does
not publish another set tick.

Required overloads propagate the weakest observed input guarantee. Optional
overloads produce ordinary outputs. Boolean outputs publish on each evaluation
unless their resolved guarantee suppresses an equal result.

Set operations and equality process changed keys incrementally. Initial
evaluation and recovery enumerate current membership. Equality retains a
mismatch count; neither implementation retains a second set of keys.

## Wiring syntax

C++ wiring points support binary `+`, `-`, `*`, `/`, `%`, `|`, `&`, `^`, unary `+` and `-`,
and `==`, `!=`, `<`, `<=`, `>`, `>=`. Use `operators::floordiv()`,
`operators::pow()`, `operators::round()`, and `operators::abs()` for operations
without C++ wiring syntax.
These operations also accept scalar operands and return `wp<>`.

For Boolean graph operations, C++ `!`, `&&`, and `||` call `not_`, `and_`, and
`or_`. Named calls are also available:

```cpp
auto bounded = (x > i64{0}) && (x < i64{10});
auto negated = !bounded;
```

Python uses `/`, `//`, `%`, and `**` for division, floor division, modulo, and
power. `round(point, ndigits)` and `abs(point)` call their corresponding
operators. Python uses the named Boolean operators because its `and`, `or`,
and `not` keywords cannot be overloaded. Write two comparisons explicitly:

```python
bounded = bg.operators.and_(x > 0, x < 10)
negated = bg.operators.not_(bounded)
```

Boolean graph operations do not short-circuit. C++ evaluates both operand
expressions during wiring. Every time series operand must be valid before the
node evaluates.

C++ `wp<T>` and `wp<>` do not convert to native `bool`. Python truth conversion
raises `TypeError`. Test output presence with `point.has_output()` in C++ or
`point.has_output` in Python.

See the [implementation guidelines](../library-implementation.md) when adding an
overload, and the [operator contract](operators.md) for selection and extension.

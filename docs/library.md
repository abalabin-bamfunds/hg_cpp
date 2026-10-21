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
| `mul` | `i64`, `double` |
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
String operations process bytes, including embedded NUL bytes. Temporal
operators are native C++ overloads. Python `datetime.timedelta` maps to
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
inferred output guarantee.

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

## Timed operators

`delay(value, duration)` publishes each input event at its original evaluation
time plus `duration`. It retains ticks, including invalidation and collection
deltas, rather than sampling the value later. Equal ordinary ticks remain
separate events. Zero duration returns the input; negative duration is a wiring
error. Positive delay limits const guarantees to final, including contained
time series. Retained ticks follow the supported types and ownership rules in
[retaining ticks](time-series-types.md#retaining-ticks). Delay rejects contained
references and sparse TSL values. TSS and TSD root invalidation cannot be
replayed, so retargeting from a valid TSS or TSD to an invalid target fails.
Initially unbound references are supported.

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

An ordinary call requires a valid collection and valid immediate items.
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
least one is true. They accept one collection argument. An ordinary call
requires a valid root and valid immediate items. `opt(values)` selects an
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

String and symbol overloads count bytes with `len_` and test substrings with
`contains_`, including embedded NUL bytes. Both substring operands have the
same scalar type. These overloads accept scalars and time series.

Runtime queries require valid time series inputs and propagate their weakest
observed guarantee. An initially invalid collection does not produce a result.
Ordinary inputs can publish equal query results on successive changes;
deduplicated outputs suppress equal results.

Use named calls in Python. Its `len()` and `in` syntax cannot return wiring
points. Python annotations for scalar lists and sets are not yet supplied.

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

See the [implementation guidelines](library-implementation.md) when adding an
overload, and the [operator contract](operators.md) for selection and extension.

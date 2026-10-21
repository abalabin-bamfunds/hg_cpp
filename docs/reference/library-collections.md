# Collection operators

Use collection operators to query or summarize current items. They do not accumulate successive input events. The code fragments use existing collection connections and direct Python imports or C++ namespace imports.

## Collection reductions

`sum_(values)`, `min_(values)`, `max_(values)`, and `mean(values)` calculate
over the current items of a numeric fixed-size TSL, TSD, or TSS. They accept `i64` and
`double` items. Sums and extrema retain the item scalar type. `mean` returns
`double` and accumulates in `double`; integer sums use the wrapping policy
described above.

An ordinary call requires a valid collection and valid immediate items.
`opt(values)` selects an optional overload that skips invalid items:

```cpp
auto complete_total = sum_(values);
auto available_total = sum_(opt(values));
```

```python
complete_total = sum_(values)
available_total = sum_(opt(values))
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
immediate item, following the [valid-item rules](input-views.md#collection-views).
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
auto empty = is_empty(available);
```

```python
shared = lhs & rhs
available = opt(lhs) | opt(rhs)
empty = is_empty(available)
```

Set results publish only actual membership changes, including a first valid
empty result. Updating one operand without changing the result membership does
not publish another set tick.

Required overloads propagate the weakest observed input guarantee. Optional
overloads produce ordinary outputs. Boolean outputs publish on each evaluation
unless their resolved guarantee suppresses an equal result.




## Optional set operations

`opt(lhs) | opt(rhs)` requests overloads treating invalid inputs as empty sets.
Marking either operand selects that contract for both. It creates no startup
evaluation and no fallback node. The same rule applies to the other optional
set operations described above.

## getitem and getattr

`collection[key]` selects one TSL, TSB, or TSD item. A scalar key fixes the
selection during construction; supported time series keys can retarget it.
`getattr(bundle, field)` selects a named field. A missing dynamic dictionary key
has no current item value. Selection follows the selected item's updates and
validity. Python object scalar overloads use their annotated result types;
see [scalar values](scalars.md#python-object-scalars).

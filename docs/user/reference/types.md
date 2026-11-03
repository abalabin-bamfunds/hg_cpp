# Types and guarantees

To state which values a calculation accepts and produces, declare its scalar types
and connection shapes. Use explicit time series types when a declaration needs views,
independently changing items, references, or update guarantees.

## Declarations

Choose the shape according to what changes together: one value, fixed positions,
named fields, a changing set of keys, or a selected source:

| Value | Python | C++ |
| --- | --- | --- |
| Integer time series | `TS[int]` | `TS<i64>` |
| Floating-point time series | `TS[float]` | `TS<double>` |
| Boolean time series | `TS[bool]` | `TS<bool>` |
| String time series | `TS[str]` | `TS<string>` |
| Fixed positions | `TSL[TS[int], 2]` | `TSL<TS<i64>, 2>` |
| Named fields | `TSB[Sensor]` | `TSB<Sensor>` |
| Dynamic keyed values | `TSD[int, TS[int]]` | `TSD<i64, TS<i64>>` |
| Membership | `TSS[int]` | `TSS<i64>` |
| Reference | `REF[TS[int]]` | `REF<TS<i64>>` |

A time series retains its current value and last update time, not a history. History
belongs to a node such as `lag` or `sums`. A scalar collection inside `TS` updates as
one value; a time series collection has independently updating items. Collection types
can nest.

Python non-variadic TSL declarations require a fixed size. C++ also has sparse TSL
forms; their operations and delta support differ from fixed lists. The standard
reductions documented here support fixed TSL only.

## Guarantees

If a calculation relies on how often a value can update, declare a guarantee to
express that requirement and let HG check it:

| Guarantee | Publication contract | Use |
| --- | --- | --- |
| ordinary | May publish repeatedly, including equal values | General changing data |
| deduplicated | Suppresses equal requested values | Downstream work only for changes |
| final | At most one valid publication; may remain invalid | A value resolved once |
| const | One publication at the start when observable | Fixed graph values |

Python supplies a guarantee as the last annotation argument, such as `TS[int, final]`.
C++ uses `TS<i64, TimeSeriesGuarantee::final>`. Collections can qualify the root and
their contained types.

An explicit input guarantee is a minimum. Stronger sources can bind to weaker
requirements: const, final, deduplicated, ordinary form the strength order. An omitted
input guarantee accepts the source's observed guarantee. A final input cannot bind to an
ordinary source merely because the example happens to send it one value.

Operators infer guarantees from their contracts. `dedup` provides at least deduplicated;
delayed constants become final because publication can occur later. A guarantee
constrains both values and publication timing; it is not an instruction to sample or to
discard history.

Invalidation removes a current value. Ordinary and deduplicated time series can recover
with a new value. Final and const mutation restrictions still apply. A collection root's
guarantee also constrains its contained time series.

## References

To follow a source that can be selected dynamically, use `REF<TS_T>` to hold the
identity of a compatible target time series. The reference's guarantee describes
retargeting, while the target has its own guarantee. Retargeting can expose a different
current value even when each target has a stronger guarantee.

Value-taking parameters dereference automatically. Explicit reference views preserve
reference identity. Reference validity and target validity are distinct. An unbound
reference has no target; a bound target can have no current value.

## Compatibility

To connect a result to another calculation, its type must satisfy the receiving
input's contract. Scalar compatibility is exact for built-in numeric types; an integer does not
automatically become a float to select an arithmetic overload. Use
[`convert`](library-numeric.md#convert). Collection compatibility checks keys, item
types, schema fields, and guarantees. A selection operation requires a common time
series type for its candidate inputs. Generic mappings resolve before evaluation;
failures identify the receiving signature and arguments.

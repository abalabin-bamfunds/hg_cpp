# Updates and literal forms

An update describes what to change at one time. It does not replace an entire
time series collection unless the type's operation explicitly copies a complete
value.

| Operation | Python | C++ literal |
| --- | --- | --- |
| No input update | `None` | `SKIP` |
| Invalidate a supported time series | `INVALID` | `INVALID` |
| Remove a dictionary key | `{key: REMOVE}` | `{{key, REMOVE}}` |
| Add set members | `{1, 2}` or `SetDelta(added={1, 2})` | `{1, 2}` |
| Remove set member | `{Removed(1)}` or `SetDelta(removed={1})` | `{REMOVED(1)}` |
| Update list position | `{0: 18}` | `{{0, 18}}` |
| Update bundle field | `{"reading": 18}` | `{{"reading", 18}}` |
| Update dictionary item | `{1: 18}` | `{{1, 18}}` |

C++ examples import `using namespace literal;`. The `_n` named-argument suffix
belongs to `bg::literals`, a separate namespace brought into scope by `bg`'s
public declarations in the guide setup.

Omitted collection items remain unchanged. Empty collection deltas do not clear
previous items. Nested deltas follow the declared child type. A scalar dictionary
inside `TS` remains one scalar; a dictionary delta applies only to `TSD`.

`INVALID` is supported for TS, fixed TSL, and TSB roots and for their compatible
items. TSS and TSD roots cannot be invalidated through these deltas. Use member
or key removals to clear them. Removing a TSD key removes its item, whereas
invalidating an item retains the key.

## Recorded results

Python returns scalar values and collection deltas. TSS results use `SetDelta`.
Invalidated contained items use `INVALID`; removed dictionary keys use `REMOVE`.
Both no root output update and root invalidation record as `None`. Observe
`valid` explicitly when a test must distinguish those events.

C++ `TestGraphResult.equals({...})` compares recorded deltas using literal forms.
`SKIP` requires no update and `INVALID` requires invalidation. A type can be
explicit on `.equals<T>(...)` when result inference needs it. See
[testing](testing.md) for typed input sequences and comparison behavior.

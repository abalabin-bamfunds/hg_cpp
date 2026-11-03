# Input views

If a calculation needs validity or change information as well as a current value,
request an input view. The view provides that access for the duration of the callback.

## Scalar values

To distinguish a fresh event from a retained reading, inspect the view's modification
and time queries along with its current value:

| Query | Python | C++ |
| --- | --- | --- |
| Current value exists | `input.valid` | `input.valid()` |
| Changed in this evaluation, including invalidation | `input.modified` | `input.modified()` |
| Last change time, including invalidation | `input.last_modified_time` | `input.last_updated()` |
| Current scalar value | `input.value` | `*input` or `input.value()` |
| Current update | `input.delta_value` | Type-specific change access |

Check validity before reading an optional input, even when `modified` is true.
On an unchanged valid scalar, current value access still returns its retained
value; delta access has no new scalar update.

Views, lazy values, and iterators expire when the callback returns. Materialize an
owning copy before storing data outside it. In Python, collection views provide `copy()`
and standard conversion such as `dict(view)`. C++ ownership uses the declared scalar's
owning representation or `ScalarValue`.

## Collection views

To process partial updates or read individual items, use a collection view.
Collection root validity and item validity are separate. A valid collection can contain
invalid items. Changes expose the current cycle's delta; cumulative access exposes
current membership and values.

| Type | Python access | C++ access |
| --- | --- | --- |
| Fixed TSL | `input[index]`, child iteration, `keys/values/items` | `input[index]`, child access and traversal |
| TSB | `input.field`, `input[name]`, `keys/values/items` | `input->field`, named child access |
| TSD | Mapping access, `key_set`, key/value/item traversal | `input[key]`, `contains`, `key_set`, `for_each_key_value` |
| TSS | Set-like `value`, `added()`, `removed()`, `was_added/was_removed` | Current membership and added/removed traversal |

Python fixed TSL and TSB expose `modified_*()` and `valid_*()` traversals.

### Dictionary change selections

If a node should process only changed dictionary items, choose the selection that
matches its work. TSD separates new keys, changed existing keys, and removals:

| Selection | Items visited |
| --- | --- |
| `added` | New keys, including keys with invalid child time series |
| `updated` | Existing keys whose child time series changed, including invalidation |
| `modified` | Added and updated keys, each visited once |
| `removed` | Removed keys |

For each selection, Python provides `<selection>_keys()`, `<selection>_values()`, and
`<selection>_items()`. Values are child input views; items are `(key, child)` pairs.
C++ provides `for_each_<selection>_key`, `for_each_<selection>_value`, and
`for_each_<selection>_key_value`. The shorter `for_each_added`, `for_each_updated`,
`for_each_modified`, and `for_each_removed` methods visit keys.

Use `modified` to process additions and updates together. Use separate `added` and
`updated` traversals when they require different handling.
A removed key is separate from an existing key whose child becomes invalid.
TSD delta mappings use `REMOVE` for removals and `INVALID` for invalid child updates.

If unavailable items should be excluded from traversal, request a `valid_items` view.
An accepting Python dictionary parameter uses `Annotated[TSD[K, V], valid_items]`;
C++ accepts `valid_items` on the corresponding `In<>` declaration. Calling
`valid_items(point)` requests that view. It exposes only valid immediate items, with
validity changes appearing as key additions or removals. Its physical `key_set`
remains the underlying membership set.

## References

If a node needs the identity of a selected source, request an explicit `REF` input.
A value input follows the target automatically. The target may update or become
invalid without the reference changing targets, so check the validity of the value
being read rather than inferring it from the presence of a reference object.

## Requiring complete items

If a partial record cannot produce a correct result, use `all_valid` to require
the root and every immediate item to be valid. This constraint does not recursively
check every descendant. The [complete-item guide](../guide/advanced-nodes/inputs.md#requiring-complete-items)
provides a Python and C++ example with partial updates, invalidation, and recovery.

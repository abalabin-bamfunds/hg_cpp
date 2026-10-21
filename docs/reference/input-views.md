# Input views

Input views provide callback-scoped access to the current value and its change
information. Use them when a calculation needs more than plain values.

## Scalar values

| Query | Python | C++ |
| --- | --- | --- |
| Current value exists | `input.valid` | `input.valid()` |
| Valid update in this evaluation | `input.modified` | `input.modified()` |
| Last valid update time | `input.last_modified_time` | `input.last_updated()` |
| Current scalar value | `input.value` | `*input` or `input.value()` |
| Current update | `input.delta_value` | Type-specific change access |

Check validity before reading an optional input. `modified` is not an invalidation
detector. On an unchanged valid scalar, current value access still returns its retained
value; delta access has no new scalar update.

Views, lazy values, and iterators expire when the callback returns. Materialize an
owning copy before storing data outside it. In Python, collection views provide `copy()`
and standard conversion such as `dict(view)`. C++ ownership uses the declared scalar's
owning representation or `ScalarValue`.

## Collection views

Collection root validity and item validity are separate. A valid collection can contain
invalid items. Changes expose the current cycle's delta; cumulative access exposes
current membership and values.

| Type | Python access | C++ access |
| --- | --- | --- |
| Fixed TSL | `input[index]`, child iteration, `keys/values/items` | `input[index]`, child access and traversal |
| TSB | `input.field`, `input[name]`, `keys/values/items` | `input->field`, named child access |
| TSD | Mapping access, `key_set`, key/value/item traversal | `input[key]`, `contains`, `key_set`, `for_each_key_value` |
| TSS | Set-like `value`, `added()`, `removed()`, `was_added/was_removed` | Current membership and added/removed traversal |

Python fixed TSL and TSB expose `modified_*()` and `valid_*()` traversals. TSD also has
`added_*()` and `removed_*()` traversals. A removed dictionary key is separate from an
existing key whose item becomes invalid. TSD delta mappings use `REMOVE` for removals
and `INVALID` for invalid item updates.

An accepting Python dictionary parameter can use `Annotated[TSD[K, V], valid_items]`.
C++ accepts `valid_items` on the corresponding `In<>` declaration. Calling
`valid_items(point)` requests that view. It exposes only valid immediate items; validity
changes appear as key additions/removals. Its physical `key_set` remains the underlying
membership set.

## References

A value input follows a reference target automatically. Explicit `REF` inputs access
reference values. A target may update or become invalid without the reference itself
changing targets. Check the validity of the value being read, rather than inferring it
from the presence of a reference object.

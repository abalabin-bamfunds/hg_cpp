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


## Requiring complete items

`all_valid` prevents evaluation until a collection root and every immediate item are
valid. This is useful when a partial record cannot produce a correct result.
A valid root alone permits missing child values.

<!-- runnable-example -->
::::::{tab-set}
:sync-group: language

:::::{tab-item} Python
:sync: python

```python
from bg import INVALID, TS, TSL, compute_node, graph, run_graph

@compute_node(all_valid=("values",))
def total(values: TSL[TS[int], 2]) -> TS[int]:
    return sum(item.value for item in values)

@graph
def complete_total(values: TSL[TS[int], 2]) -> int:
    return total(values)

print(run_graph(complete_total, [{0: 10}, {1: 20}, {0: INVALID}, {0: 5}]))
```

Output:

```text
[None, 30, None, 25]
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

const auto total = node([](In<TSL<TS<i64>, 2>, bg::all_valid> values) {
    return *values[0] + *values[1];
});

auto complete_total(wp<TSL<TS<i64>, 2>> values) {
    return total(values);
}

int main() {
    const auto result = run_graph(complete_total, {{{0, 10}}, {{1, 20}}, {{0, INVALID}}, {{0, 5}}});
    std::cout << std::boolalpha << result.equals({std::nullopt, 30, std::nullopt, 25}) << '\n';
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

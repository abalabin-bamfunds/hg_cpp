# Graph instances and collections

When each item or selected case needs its own state, timers, or lifecycle, use an operation that
creates runtime graph instances. Each instance owns those resources. Use ordinary graph calls when a
runtime boundary is not required. C++ graph arguments need a concrete output type. Use
`.as<TS<T>>()` when a library expression returns an untyped connection.

Python imports `map`, `mesh`, `switch_`, `reduce`, and `pass_through` from `hgraph`. C++ imports
`hg` and includes the corresponding `<hg/map.hpp>`, `<hg/mesh.hpp>`, `<hg/switch.hpp>`,
or `<hg/reduce.hpp>` header.

## map

If you need the same computation for every item in a collection, use
`map(graph, arguments...)` to apply a graph per selected key. A mapped argument is a TSD
or a `TS<Dict<K, V>>` whose item feeds that branch parameter. A scalar Dict tick supplies
a complete snapshot; map scans it and updates only branches whose values changed when
the item type supports equality. `pass_through(point)` supplies the whole time
series to each branch. C++ programs that map a scalar Dict must link `hg::library`, which
provides the Dict-to-TSD conversion overload. Scalars remain fixed arguments. A TSS can
supply keys for a source-like graph.

If branch membership should follow one input rather than the union of mapped
dictionaries, set `__key_set_arg__` to that argument's index or name. To let the branch
use its own key in calculations, set `__key_arg__` to the parameter that should receive
it as a const time series. Python uses named keywords; C++ uses
`"__key_arg__"_n = ...` and corresponding named arguments.

Adding a key creates and starts its branch. Removing it stops the branch and removes its
result. Re-addition creates fresh state. Missing inputs for a key are invalid, so
required node constraints can prevent branch calculations. A calculation graph returns a
dictionary of result references. A sink graph returns no output; a source graph uses its
selected keys to create outputs.

## switch_

If each operating mode needs a different calculation, use
`switch_(key, cases, arguments...)` to run the selected case graph. Python cases use a
dictionary, with tuple keys for multiple keys in one case. C++ uses `cases{case_(key,
graph), ...}`. `DEFAULT` selects fallback behavior for invalid or unmatched keys.
Without a fallback those keys raise an error.

Changing to another case stops the previous instance and starts a fresh one. A different
key selecting the same case retains its instance. Current arguments are observable when
a case starts. Outputs are references to the active graph; source, sink, and outputless
fallback forms are supported according to case compatibility. Use value selection when
both computations should continue.

## mesh

For per-key calculations that depend on other keys' results, use
`mesh(graph, arguments..., name=...)`. It follows map's argument and key rules. Within a
branch, `mesh()[key]` requests another branch's output and records its dependency.
`mesh("name")[key]` selects a named enclosing mesh.

Dependencies evaluate before their consumers. A requested missing key creates a
dependency branch, even outside normal mapped membership. That branch remains while a
dependent needs it. Changing keys retargets subscriptions. Removed or invalid branch
outputs make observed dependency values invalid. A dependency cycle raises an error
rather than supplying an old result.

## reduce

To combine current collection items with your own binary graph calculation, use
`reduce(graph, values, default=...)` in Python or
`reduce(graph, values, "default"_n = ...)` in C++. Both combine valid TSD items. Python accepts
a decorated `@graph` reducer or a binary callable, including a lambda. A callable receives
two wiring points during graph construction and must return a compatible wiring point.
Its input types and expected result type come from the TSD item type. Plain scalar
annotations on decorated graph inputs resolve to time-series inputs.

The reducer graph has two explicit time-series inputs and returns a time series
compatible with the item type. Scalar-valued items produce a scalar-valued result; other
item types produce a reference to the reduced time series. The scalar case accepts a
scalar default. Other item types accept a time-series default of the item type. The
operation must be associative and commutative; dictionary order is unspecified. C++ context
inputs are allowed.

One valid item is forwarded directly. With no valid items, a valid dictionary uses the
supplied default or has no result. An invalid dictionary has no result. Target changes
of referenced items contribute even when keys do not change. Reducer inputs and output
use ordinary guarantees because their tree positions can receive different items. Do not
depend on a particular reduction tree or wiring-call count.

Omit `default` when an empty reduction should have no result. Python `None` is not a
default value. The [custom aggregation guide](../guide/graphs/aggregation.md#custom-aggregation)
shows a product with an empty-collection default.

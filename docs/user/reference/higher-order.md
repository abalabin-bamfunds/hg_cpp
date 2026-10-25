# Graph instances and collections

Higher-order operations create runtime graph instances. Their state, timers, and
lifecycle belong to those instances. Use ordinary graph calls when a runtime boundary is
not required. C++ graph arguments need a concrete output type. Use
`.as<TS<T>>()` when a library expression returns an untyped connection.

Python imports `map`, `mesh`, `switch_`, and `pass_through` from `bg`. C++ imports
`bg` and includes the corresponding `<bg/map.hpp>`, `<bg/mesh.hpp>`, or
`<bg/switch.hpp>` header.

## map

`map(graph, arguments...)` applies a graph per selected key. A mapped argument is a TSD
whose item feeds that branch parameter. `pass_through(point)` supplies the whole time
series to each branch. Scalars remain fixed arguments. A TSS can supply keys for a
source-like graph.

The default key set is the union of mapped dictionaries. `__key_set_arg__` selects a
particular mapped argument by index or name. `__key_arg__` identifies the branch
parameter receiving its key as a const time series. Python uses named keywords; C++ uses
`"__key_arg__"_n = ...` and corresponding named arguments.

Adding a key creates and starts its branch. Removing it stops the branch and removes its
result. Re-addition creates fresh state. Missing inputs for a key are invalid, so
required node constraints can prevent branch calculations. A calculation graph returns a
dictionary of result references. A sink graph returns no output; a source graph uses its
selected keys to create outputs.

## switch_

`switch_(key, cases, arguments...)` runs the selected case graph. Python cases use a
dictionary, with tuple keys for multiple keys in one case. C++ uses `cases{case_(key,
graph), ...}`. `DEFAULT` selects fallback behavior for invalid or unmatched keys.
Without a fallback those keys raise an error.

Changing to another case stops the previous instance and starts a fresh one. A different
key selecting the same case retains its instance. Current arguments are observable when
a case starts. Outputs are references to the active graph; source, sink, and outputless
fallback forms are supported according to case compatibility. Use value selection when
both computations should continue.

## mesh

`mesh(graph, arguments..., name=...)` follows map's argument and key rules. Within a
branch, `mesh()[key]` requests another branch's output and records its dependency.
`mesh("name")[key]` selects a named enclosing mesh.

Dependencies evaluate before their consumers. A requested missing key creates a
dependency branch, even outside normal mapped membership. That branch remains while a
dependent needs it. Changing keys retargets subscriptions. Removed or invalid branch
outputs make observed dependency values invalid. A dependency cycle raises an error
rather than supplying an old result.

## reduce

C++ `reduce(graph, values, "default"_n = scalar)` combines valid TSD items. The reducer
graph has two explicit time series inputs and a scalar-valued output compatible with the
item type. Its operation must be associative and commutative; dictionary order is
unspecified. Context inputs are allowed.

One valid item is forwarded directly. With no valid items, a valid dictionary uses the
supplied default or has no result. An invalid dictionary has no result. Target changes
of referenced items contribute even when keys do not change. Reducer inputs and output
use ordinary guarantees because their tree positions can receive different items. Do not
depend on a particular reduction tree or wiring-call count.

Python does not currently expose `reduce`. Use built-in collection reductions or a
custom collection node. The [custom aggregation guide](../guide/graphs/aggregation.md#custom-aggregation) shows the
same scalar calculation through the available APIs.

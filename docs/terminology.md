# Terminology

This file defines project-specific terms and their required use.

## Change process

Before adding a term, discuss these items with the project owner:

- term
- definition
- when to use it
- why the project needs it

Add the term only after explicit approval. The same rule applies to a changed or
removed entry.

Names that come from C++, CMake, Conda, Python, or another external specification
keep their standard meaning. They need an entry only when this project gives them
a narrower meaning.

## Usage rule

Use a term only when the subject meets its complete definition. Do not use the
term for one part of the defined entity or for a related entity. An entry has a
specific usage note only when this rule is not sufficient.

Order terms from simple to dependent. A term must appear after the terms needed
to define it.

## Approved terms

### scalar

A value that the engine treats as one value. A scalar can be an integer, a
floating-point number, a string, a list of scalars, a key-value dictionary, or
another supported value.

In this project, a scalar is not limited to a number or to a value that cannot
contain other values.

### compound scalar

A scalar with an ordered schema of named fields and exact type compatibility.

### hierarchical scalar

A scalar with an ordered schema of named fields and explicitly declared base
types. It accepts values of its declared type and derived types. Compatibility
is covariant through containers.

### runtime type

An object that identifies a semantic type, describes one physical layout, and
provides the operations required to interpret and manage data in that layout.
One semantic type can have multiple runtime types.

### finalization

Cleanup of a stored value that must run even when its owner discards the complete
memory resource. This includes releasing external resources and required
application-specific destruction effects.

### time series

A value that can change over time. A time series is valid when it has a current
value. A valid time series contains its current value and its last updated time.

In this project, a time series does not contain a history of past values.

`TS_T` represents any one time series type.

### invalidation

The removal of the current value from a valid time series. The time series
remains present.

### time series guarantee

A constraint on value mutation and publication for a time series. Every time
series type has one guarantee. The guarantees are `ordinary`, `deduplicated`,
`final`, and `const`.

For a collection time series, the guarantee also applies to every contained
time series.

### ordinary time series

A time series with no constraint on the number or values of its updates.
`ordinary` is the default time series guarantee.

### deduplicated time series

A time series that suppresses a requested mutation when its value equals the
current value. Each accepted mutation changes the current value. Later
mutations in the same node invocation do not cancel its publication.

### final time series

A time series that updates at most once. It can be invalid before its update or
remain invalid for its complete lifetime.

### const time series

A time series that updates once at its start time and never again. Its start
time is the evaluation time at which it becomes observable.

### time series value

A time series whose current value has type `T`, written `TS<T, G>` when its
guarantee `G` is relevant. `TS<T>` uses the `ordinary` guarantee.

### sampling

Treating the current value of a time series as a new value at the sampling time,
without updating the source time series. The sampled view reports the sampling
time as its last updated time.

### time series reference

A time series whose value identifies another time series without copying the
target value, written `REF<TS_T>`. It has its own last updated time and directly
identifies its target time series.

### time series set

A time series value that holds a set of scalar items of type `T`, written
`TSS<T>`. Its interface reports the items added and removed by an update.

### collection time series

A time series whose items are also time series. It has its own last updated time
and a cumulative value composed from the current value of each item.

Cumulative means composed from current item values. It does not mean a sum or a
history of past values. The collection can have a fixed or dynamic size.

### time series list

A collection time series whose items have the same time series type.
`TSL<TS_T, Size>` has a fixed size. `TSL<TS_T>` has a variable set of occupied
indices and is sparse. The fixed-size form reports updated items. The sparse
form reports added, removed, and updated items.

### time series bundle

A fixed-size collection time series whose named items can have different time
series types, written `TSB`. A bundle can include the items defined by
another bundle and add named items. Its interface reports the items updated by
an update.

### time series dictionary

A dynamically sized collection time series that maps scalar keys of type `K` to
time series of one type `TS_T`. Write it as `TSD<K, TS_T>` when the key and item
types are relevant. It consists of a `TSS<K>` and a `TSL<TS_T>`. The set maps
each key to the sparse list index of its value. Its interface reports added and
removed keys and modified items.

### node

A function that performs a computation, together with information that identifies
the source of each input. A node produces an output but does not record how that
output is used.

### graph code

Code that constructs a graph by creating nodes and connecting their inputs and
outputs. The connections define how data moves through the graph.

### graph function

A function that constructs graph code by invoking node templates or graph
templates.

### parameter

A named input or scalar argument in a node or graph signature. Wiring resolves
a plain C++ function parameter to a time series input or a scalar from the
corresponding graph-code argument. A parameter declared with `In<TS_T, ...>` is
a time series input.

### active input

A node input that schedules the node for evaluation when its time series changes.

### passive input

A node input that does not schedule the node when its time series changes. The
node reads its current state when another input or the engine schedules
evaluation.

### timed scheduling

Scheduling a node for evaluation at a specified later evaluation time.

### input validity constraint

A rule on a node input that states whether its time series must be valid before
the node can be evaluated. An input that requires validity prevents evaluation
while it has no current value.

The constraint is `required`, `optional`, or `all_valid`. A required input must
be valid. An optional input does not prevent evaluation while it is invalid.
An `all_valid` input requires its root and every immediate contained time series
to be valid. These words describe validity; every input has a source.

### `valid_items` guarantee

An input guarantee for a time series dictionary. An input view with this
guarantee exposes only keys whose item time series are valid. It applies to the
immediate items of the time series dictionary.

The view reports an item becoming valid as an added key and an item becoming
invalid as a removed key.

### signature

The contract of a node or graph. It specifies the name and type of each
parameter, any constraint on its resolved category, its input activity and
validity constraint when applicable, and zero or one output time series type. A
parameter category or type can be generic at this stage.

A generic category or type is a placeholder that must be replaced before the
node is wired.

### resolved signature

A signature in which every parameter is classified as a time series input or
scalar and every type has been resolved. It contains no generic category or
type placeholders.

### wiring template

The common graph-code interface of node templates and graph templates. It
provides a signature and defines how a checked call contributes to graph
wiring.

### node template

A signature paired with the code that implements evaluation, start, and stop.

In this project, `node template` does not mean a C++ language template.

### graph template

A signature paired with a graph function. Calling it checks its boundary and
then invokes the graph function in a named wiring context.

### operator

A named graph-code callable that selects one registered node template or graph
template from the supplied arguments. Its prototype describes its intended use
and whether it produces an output.

### operator overload

A node template or graph template registered as one implementation of an
operator. Its signature can differ from the operator prototype and from other
overloads.

### wired node

A node template whose signature is resolved, together with scalar values
supplied while the graph is constructed and an identifier for each time series
input. Each identifier selects a local wired output or an inherited context
requirement. The scalar values are immutable after graph construction. A wired
node does not know how its output is used.

### wired output

The compiler-facing identity of a wired node's root output or one of its
contained outputs. The C++ type is `WiredOutput`.

### graph wiring

The mutable graph-construction state that owns wired nodes, wired outputs, and
their connections before graph compilation. The C++ type is `GraphWiring`.

### wiring point

A pointer-sized value that identifies a wiring record owned by a `GraphWiring`.
The C++ type is `WiringPoint`. `wp<TS_T>` adds a time series type for
graph-signature inference without adding runtime storage.

### wiring context

A scope that makes one `GraphWiring` active for node calls on the current
thread. Nested wiring contexts restore the previous active `GraphWiring` when
they end. The C++ type is `WiringContext`.

### context time series

A named time series available during graph wiring. A lexical binding or an
explicit named argument provides its value. A graph or node parameter requests
it by using the same name and a compatible time series type.

### wiring path

The chain of names and labels that identifies where a wired node was created in
graph code. Each entry refers to its parent entry. Labels describe calls and
can repeat.

### compiled node

Immutable execution data produced for one wired node after the complete wired
graph has been processed. It contains the runtime storage offsets and scheduling
data derived from the complete graph. Runtime graphs with the same wiring share
it.

### runtime node

The mutable state that one runtime graph uses with one compiled node. Each
runtime graph has separate state for that compiled node. The state includes an
output time series when declared and can include private mutable implementation
state.

### runtime graph

A list of runtime nodes in topological dependency order. Every runtime node
appears after the runtime nodes that it depends on. In a top-to-bottom display,
dependencies are above the runtime nodes that depend on them.

### simulation

Evaluation that advances directly to the next scheduled evaluation time instead
of waiting for wall-clock time.

### nested graph

A runtime graph owned and invoked by a node in another runtime graph. The owning
runtime graph is the outer graph. The owned runtime graph is the inner graph.

### outer cycle

One evaluation cycle of an outer graph, including all work that it schedules in
its inner graphs. It ends after all that work is complete.

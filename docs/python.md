# Python integration

The Python package provides initial graph authoring and node callbacks while
keeping Python and nanobind out of the `bg` engine target. Build it with
`BG_BUILD_PYTHON=ON`. The installed CMake package then provides `bg::python` in
addition to `bg::bg`.

## Build and install

The Python extension requires Python 3.13 or newer, its development module, and
nanobind in the selected Python environment. Python tests also require pytest.

```sh
python -m pip install nanobind pytest
cmake -S . -B build/python -DBG_BUILD_PYTHON=ON -DBUILD_TESTING=ON
cmake --build build/python
ctest --test-dir build/python --output-on-failure -R '^python_tests$'
```

The build-tree package is in `build/python/bindings/python`. A wheel can be
built with `scikit-build-core` by running `python -m pip wheel .`.

CMake consumers request the Python component with `find_package(bg COMPONENTS
python REQUIRED)` and link `bg::python` to use the Python-aware C++ support
headers and library. Consumers that do not request the component need neither
Python nor nanobind. The Python component resolves them from the Python
environment selected by CMake. A consumer building a nanobind module must also
use `nanobind_add_module(... NB_SHARED ...)` so that it shares nanobind's runtime
with `bg::python`.

## Graph authoring

The package exports time-series annotations, node and graph decorators,
`test_graph`, engine operators, `Symbol`, and `symbol`.

```python
import bg
from typing import TypeVar

@bg.source_node
def source() -> bg.TS[int]:
    return 1

@bg.compute_node
def add(value: bg.TS[int], amount: int = 2) -> bg.TS[int]:
    return value.value + amount

@bg.graph
def example() -> bg.TS[int]:
    return add(source())

assert bg.test_graph(example) == [3]
```

Use standard `typing.TypeVar` declarations for generic graph signatures. An
unbounded or scalar-constrained TypeVar represents a scalar type; use it inside
`TS[...]` or as a plain scalar graph parameter. Bind a TypeVar to
`bg.TimeSeries` when it represents an entire time-series type:

```python
VALUE = TypeVar("VALUE")
SERIES = TypeVar("SERIES", bound=bg.TimeSeries)

@bg.graph
def identity(value: bg.TS[VALUE]) -> bg.TS[VALUE]:
    return value

@bg.graph
def forward(value: SERIES) -> SERIES:
    return value
```

A whole-time-series TypeVar captures the complete resolved type, including its
root guarantee, so repeated occurrences require that same type.

TypeVar identity determines which occurrences share a type mapping. Names are
descriptive only, so separate TypeVar objects with the same name remain
independent. `bg.TimeSeries` is a typing marker for the TypeVar bound; it is not
a value annotation.

Specialize a generic graph with its fully resolved input types. The resulting
graph definition can be called from normal wiring and passed to `test_graph`:

```python
specialized = graph[bg.TS[int], bg.TSD[int, bg.TS[str]]]
```

`bg.operators` is the shared namespace for engine and library operators,
including higher-order graph operators. Native operators use C++ overload
resolution for both explicit calls and wiring-point special methods.
Wiring-point arithmetic and comparison methods call the native operators and
preserve operand order. See the [standard library](library.md) for supported
types and wiring syntax.

Wiring points support `/`, `//`, `%`, and `**`; `round(point, ndigits)` and
`abs(point)` call the corresponding operators. `bg.operators.convert` takes an
explicit full result type descriptor:

```python
converted = bg.operators.convert[bg.TS[float]](integer_value)
```

The built-in conversions support `int` and `float`. C++ overload registration
can extend `convert` to other time-series inputs and collection outputs.
Positional and named arguments can contain wiring points or registered Python
scalar values. A sink operator returns `None`. The higher-order operators are
the same objects as `bg.map`, `bg.mesh`, and `bg.switch_`.

```python
@bg.graph
def constant() -> bg.TS[int]:
    return bg.operators.const_(value=42)

item = collection[key]  # bg.operators.getitem(collection, key)
```

`getitem` and `getattr` also accept Python-object time series. `getitem` accepts
a scalar or time-series key. `getattr` accepts a scalar string or symbol name.
The output scalar type is inferred from the Python `__getitem__` return
annotation or the selected attribute's annotation; provide an explicit output
type when inference is unavailable, for example
`bg.operators.getitem[int](value, key)` or
`bg.operators.getattr[int](value, "count")`. The bracketed type is the scalar
value type; these overloads return `TS` of that type.
Time-series collection overloads do not accept a bracketed output type.

`bg.opt(point)` requires an optional receiving parameter. It returns a new
wiring proxy for the same time series and leaves `point` unchanged. It adds no
runtime node or fallback value. The selected node defines how it handles an
invalid input. See [optional arguments](operators.md#optional-arguments).

`Annotated[TSD[K, V], bg.valid_items]` declares a filtered input view.
`bg.valid_items(point)` requires that annotation on the receiving parameter:

```python
from typing import Annotated

@bg.compute_node
def count(values: Annotated[bg.TSD[int, bg.TS[int]], bg.valid_items]) -> bg.TS[int]:
    return len(values)

count(bg.valid_items(values))
```

The annotation adds no runtime node and leaves the original wiring point
unchanged. Its [input-view behavior](time-series-validity.md#collections)
matches C++, including immediate-item validity and physical `key_set` access.

Node and graph functions accept positional and named arguments according to
their Python signatures, including positional-only, keyword-only, `*args`, and
`**kwargs` parameters. A graph function expands into the active graph while it
is being wired; it does not create a runtime nested-graph boundary.

For `@graph` and `@node`, a plain scalar annotation such as `int` accepts an
`int` scalar or a `TS[int]` wiring point. A graph function receives the scalar
value or wiring point supplied by its caller. `Scalar[int]` restricts a
parameter to an `int` scalar. In `@graph`, explicit time-series annotations
remain time-series-only and can be mixed with plain scalar annotations. `@node`
parameters must use scalar annotations rather than `TS[...]`. For
`@compute_node`, a plain scalar annotation remains scalar-only.

`Optional[T]` lets a plain graph or node parameter accept an absent scalar or
an optional time-series input. A graph receives `None` for an absent scalar;
it receives an optional wiring point for a time-series argument. A node
callback receives `None` when a scalar is absent or its time-series input is
invalid. `Optional[Scalar[T]]` accepts only an absent or present scalar. A
scalar return annotation on `@graph` declares a `TS[T]` output. The
graph may return a scalar value, which is wired through `const_`, or a
compatible time-series wiring point. An explicit `TS[T]` return annotation
requires a wiring point.

Variadic parameters use a collection annotation. The function body receives
one collection rather than expanded Python arguments:

```python
@bg.graph
def homogeneous(*values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
    return values[0]

@bg.graph
def positional(*values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
    return values[0]

@bg.graph
def named(**values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
    return values["value"]
```

`TSL[..., SIZE]` accepts homogeneous time series. A TSB accepts time series and
scalars; scalar arguments are lifted to const time series. Positional TSB fields
use integer indices and named TSB fields use their argument names. A graph
receives a wiring-time collection. A node receives a materialized input view.
A prebuilt TSL or TSB supplied with the variadic parameter's name binds as the
complete collection.

`test_graph` accepts one update sequence for every graph parameter and wires
each sequence according to the parameter's declared time-series type. Plain
scalar annotations accept this time-series form. `Scalar[T]` parameters require
scalar arguments during graph construction; call them from a graph function to
test that form. Each sequence element is a value or `None`; `None` skips that
input's update for the cycle.
Collection annotations interpret sequence elements as deltas: TSD uses key-to-
item mappings with `REMOVE` for key removal; TSS uses sets or `SetDelta`, with
`Removed(value)` marking removals; TSL uses index-to-item mappings; TSB uses
field-name-to-item mappings. Nested collection deltas follow the declared item
type. `INVALID` invalidates TS, TSL, or TSB inputs; TSS and TSD do not support
invalidation. A scalar collection such as `TS[dict[int, str]]` remains one
scalar value and is not interpreted as a TSD delta. Generic graph definitions
require explicit, fully resolved input types, for example
`test_graph(graph[bg.TS[int], bg.TSD[int, bg.TS[str]]], ...)`.
The graph can return `TS`, `TSS`, `TSL`, `TSD`, or `TSB`, including nested
collections. The result has one entry per cycle. A valid published tick becomes
a scalar value or collection delta in the same format as input deltas; TSS
results use `SetDelta`. Skipped and invalid root ticks become `None`.
Invalidated collection items use `INVALID` and removed TSD keys use `REMOVE`.

```python
@bg.graph
def identity(values: bg.TSD[int, bg.TS[str]]) -> bg.TSD[int, bg.TS[str]]:
    return values

assert bg.test_graph(identity, [{1: "first"}, {1: bg.REMOVE}, None]) == [
    {1: "first"}, {1: bg.REMOVE}, None,
]
```

`run_graph` wires a graph with normal Python arguments and selects its engine
through `engine=bg.EvaluationEngine.simulation` or
`engine=bg.EvaluationEngine.realtime`. Simulation is the default. Scalar
arguments become wiring-time constants. Use `test_graph` when the graph needs
update sequences or its output values.

`start_time` sets the first evaluation time using a timezone-naive local Python
`datetime`, as returned by `EvaluationClock`. When omitted, the defaults follow
[direct execution](graph-wiring.md#direct-execution).
`test_graph` also accepts `engine` and `start_time`; it requires simulation and
keeps result positions relative to the configured start time.

Realtime execution waits for work until `stop_token` requests cancellation.
Create a `bg.StopSource`, pass `source.get_token()` as `stop_token`, and call
`source.request_stop()` from a graph callback or another thread. Cancellation
follows the [engine lifecycle](evaluation.md#realtime-evaluation).

```python
stop_source = bg.StopSource()

@bg.node
def finish() -> int:
    stop_source.request_stop()
    return 42

@bg.graph
def realtime_example() -> bg.TS[int]:
    return finish()

bg.run_graph(realtime_example, engine=bg.EvaluationEngine.realtime,
             stop_token=stop_source.get_token())
```

`StopSource.stop_requested()` and `StopToken.stop_requested()` report whether
stop was requested. A token retains its shared stop state after its source is
destroyed. Simulation uses its normal termination condition and ignores a stop
token.

On Python's main thread, `run_graph` and `test_graph` process Python signals
through a [periodic callback](evaluation.md#periodic-callbacks). Ctrl+C raises
`KeyboardInterrupt` after graph cleanup. Realtime execution also checks during
idle and future-start waits. Checks run approximately every 50 milliseconds,
subject to completion of the current outer cycle. Python callbacks can process
signals during their own execution. Custom signal handlers retain their normal
Python behavior; a handler that returns normally lets execution continue.
Realtime graphs running on other threads use explicit stop requests for cancellation.

Pass an optional recorder created by `EvaluationProfiler.recorder()` through
the `recorder` keyword. Profiling follows the selected engine and start time.

```python
profiler = bg.EvaluationProfiler()
bg.run_graph(example, recorder=profiler.recorder())
print(profiler.report())
```

An `EvaluationProfiler` recorder records one graph run. Its `report()` formats
outer graph timing with UTC period labels. `outer_totals()` and `outer_periods()`
return outer-cycle timing. `root_graph_id()`, `graph_ids()`, and `graph(id)`
provide stable graph metadata and cumulative graph evaluation totals after the
run. `node_period_starts()` and `node_periods(graph_id, start)` query retained
node evaluation counts and checkpoint ticks. `graph_period(...)` and
`node_period(...)` query closed periods. Period lengths and retention counts
are configurable with `node_period`, `retained_node_periods`, `outer_period`,
and `retained_outer_periods`; period values are `datetime.timedelta` objects.

Wiring failures preserve an underlying Python exception's identity, type, and
traceback. Exception notes include the context described by
[wiring errors](graph-wiring.md#wiring-errors).

## Services

`@reference_service` declares a source-like service, `@subscription_service`
declares a keyed subscription, and `@service` declares a request/reply service.
Service inputs require annotations; scalar inputs use their scalar annotation
and time-series inputs use `TS[...]`. Service outputs must use an explicit
`TS[...]` annotation. An outputless `@service` declaration is a sink. Reference
and subscription services require an output. Calling a declaration wires a
service request and returns its output wiring point, or `None` for a sink.

Register graph implementations with `ServiceCatalogue.register_impl`, then
pass the catalogue to `run_graph` or `test_graph`:

```python
@bg.reference_service
def clock() -> bg.TS[int]:
    pass

@bg.source_node
def source() -> bg.TS[int]:
    return 42

@bg.graph
def clock_implementation() -> bg.TS[int]:
    return source()

catalogue = bg.ServiceCatalogue()
catalogue.register_impl(clock, clock_implementation)

@bg.graph
def client() -> bg.TS[int]:
    return clock()

assert bg.test_graph(client, catalogue=catalogue) == [42]
```

For a request/reply service, the implementation graph receives the request
time series as a `TSD` keyed by client id. When the service has an output, the
implementation returns a `TSD` delta with the same keys. A named registration
argument matching a scalar service parameter filters the implementation by
that parameter's value. Other registration arguments are passed to the
implementation graph as installation arguments. The catalogue retains the
Python service and graph wrappers while its native registrations are in use.

A subscription key uses a final-or-stronger time series, for example
`bg.TS[int, bg.final]`. Its implementation receives `bg.TSS[int]` and returns
`bg.TSD[int, R]` for the declared result type `R`. See
[graph services](services.md) for the signatures and request behavior.

## Time-series types

Python declaration types and their referenced variables follow the
[type registration contract](type-system.md#type-model). Their metadata has
process lifetime. Definition and callback ownership follows the
[runtime boundary](#runtime-boundary).

Python definitions can use these annotations:

```python
bg.TS[int]
bg.REF[bg.TS[int]]
bg.TSS[int]
bg.TSL[bg.TS[int], 4]
bg.TSD[int, bg.TS[float]]
bg.TSB[Quote]
```

A time-series annotation can add a root guarantee, for example
`bg.TS[int, bg.final]` or `bg.TSD[int, bg.TS[float], bg.deduplicated]`.
The available values are `bg.ordinary`, `bg.deduplicated`, `bg.final`, and
`bg.const_`. An explicit input guarantee is a minimum: stronger sources also
match. An omitted input guarantee accepts and retains the source guarantee.
Guarantees can also qualify concrete collection items and schema fields.
The outer collection of a variadic `TSL[..., SIZE]` or `TSB[TS_SCHEMA]`
cannot be qualified. See [time series guarantees](type-system.md#time-series-guarantees).

A non-variadic TSL requires a fixed size. A Python schema declares scalar
bundle fields and their TSB projection:

```python
from typing import Optional

@bg.schema
class BaseQuote:
    bid: float

@bg.schema
class Quote(BaseQuote):
    ask: Optional[float]
```

`bg.Bundle[Quote]` is a scalar type for schemas whose fields are all scalar.
`bg.TSB[Quote]` is a time-series bundle type. A scalar field projects to a
time series of that scalar type; an explicit time-series field passes through.
The TSB projection does not preserve optional scalar presence. The scalar
`Bundle` retains it, and the generated constructor defaults an omitted optional
field to `None`.

Explicit time-series fields pass through to the TSB projection:

```python
@bg.schema
class MarketData:
    price: float
    venue: bg.TS[str]
```

Schemas support single inheritance. Each declaration has one identity shared by
its scalar and TSB projections. The qualified Python name is descriptive.
Optional direct self-fields support recursive scalar bundles:

```python
@bg.schema
class Node:
    value: int
    child: Optional["Node"]
```

Required recursive fields are rejected because they create an unconstructible
by-value cycle. Python schema declarations do not currently support recursive
collections or mutually recursive declarations. Conversion back to Python
allocates the declared class and assigns its schema fields without calling its
custom `__init__` method.

## Python nodes

`@source_node` declares a source whose decorated function is its evaluation
callback. The engine schedules it once at graph start. It accepts scalar wiring
arguments and returns a `TS[...]` delta. It can request `Scheduler` to schedule
later evaluations of the same function. An optional `@source.start` callback
runs before the first evaluation. `@compute_node` declares an input-driven
node. `@generator` declares a timed source that yields output deltas.

`@node` and `@compute_node` use their decorated function as the evaluation
callback. Register lifecycle callbacks with `@name.start`, `@name.stop`, and
`@name.on_invalidation`. A start callback runs before evaluation and is
side-effect-only; its return value is ignored. Each callback declares a subset
of the decorated function's parameters by name; arguments are passed in the
callback's declared order. A parameter name not present on the decorated
function is an error.

```python
@bg.node
def add(value: int, offset: int) -> int:
    return value + offset

@add.start
def start(offset: int):
    initialize(offset)
```

`@generator` yields `(datetime, delta)` at an absolute evaluation time or
`(timedelta, delta)` relative to the current evaluation time. Yielded times
must increase strictly. An empty generator is valid. The engine closes the
generator when its node stops. Exceptions from stop callbacks are reported
through Python's unraisable-exception hook after stop cleanup.

Nodes can request `bg.EvaluationClock`, `bg.Scheduler`, or `bg.State[T]` as
parameters. Each injectable must have `= None` as its default and is omitted
from the wiring signature. Place positional injectables after required
positional parameters, or declare them keyword-only, because Python does not
allow a required positional parameter after a defaulted one.
An evaluation callback can also request `output: bg.Output = None`. This
provides the same read-only API as an input view, over the node's retained
output before the callback's returned delta is applied. `modified` and
`delta_value` keep their current-cycle meaning, so they are normally `False`
and `None` during evaluation. Output views are valid only during their callback;
the node publishes changes only by returning a delta. A lifecycle callback can
request the same view by selecting the `output` parameter by name. `@generator`
rejects `bg.Output` because its function parameters remain in the generator
frame between callback invocations.
`EvaluationClock.evaluation_time` exposes the evaluation time. `EvaluationClock.now`
exposes the current time for the engine mode: evaluation time in simulation mode
and wall-clock time in real-time mode. Both are Python `datetime` values.
`Scheduler.schedule_at(time)` schedules that node at a later evaluation time.
`Scheduler.schedule_wall_clock_at(time)` schedules it when the wall clock
reaches the deadline. Both return a `bg.ScheduleHandle` and follow the
[timer semantics](evaluation.md#timed-scheduling).
`Scheduler.cancel(handle)` cancels a pending handle for the same node.
`Scheduler.timer_fired(handle)` reports whether that node's timer activated in
the current outer cycle. Both injected objects are valid only while the
callback runs. A schedule handle can be retained and used by a later invocation.
Only a node that declares `Scheduler` or uses `@generator` enables timed
scheduling; `EvaluationClock` alone does not. `@source_node` can use
`Scheduler` to schedule its evaluation callback. `@generator` uses yielded
times and rejects `Scheduler`.

`State[T]` calls the zero-argument callable or class `T` once on the first
invocation of each runtime node. The callback receives that object directly.
Each runtime node, including nodes in nested branches and separate graph runs,
has independent state. A node can declare at most one `State[T]` parameter.

`@node`, `@compute_node`, and `@source_node` accept these injectables subject to
the source restrictions above. `@generator` accepts `EvaluationClock` and
`State[T]` but rejects `Scheduler` and `Output`. Graph definitions cannot
request injectables.

`@node` declares scalar-valued inputs and output. Each parameter annotated with
a scalar type accepts either a scalar wiring argument or a matching `TS[T]`
wiring point. Scalar arguments are passed unchanged. Time-series arguments are
passed as their current Python scalar value; an invalid optional time-series
input is passed as `None`. A scalar return annotation declares a `TS[T]` output.
Omitting the return annotation declares a sink. The node runs at graph start
only when all resolved inputs are scalar arguments.

```python
@bg.node
def add(value: int, offset: int) -> int:
    return value + offset

@bg.graph
def example(value: bg.TS[int]) -> bg.TS[int]:
    return add(value, 1)
```

`@node` parameters must use scalar annotations, not `TS[...]`. `Scalar[T]`
restricts a parameter to a scalar. `Optional[T]` marks a scalar argument as
optional and makes a time-series input optional if that parameter is wired to
a time series.

`dict[K, V]` declares a scalar dictionary. A plain `@node` callback receives a
read-only `bg.Dict` mapping view for a dictionary parameter, whether graph code
supplies a scalar or a `TS[dict[K, V]]`. The view expires when the callback returns.
Length and key lookup do not convert unrelated entries; `[]` and `get()` each
perform one dictionary lookup. Iterators advance a native cursor and convert
entries only as Python consumes them. Each iterator uses fixed-size storage
instead of snapshotting pointers proportional to the dictionary size. Each
iteration starts a new cursor, and the iterator remains callback-scoped.
`view.copy()` materializes the dictionary with direct native traversal.
`dict(view)` also works, but Python's mapping constructor iterates keys and
looks up each value. A dictionary output still uses an ordinary Python `dict`.
`@compute_node` exposes a dictionary through a lazy input value view.

A callback returns an output delta. Returning `None` requests no output change.
Sink nodes have no output annotation and must return `None`. An exception
raised by a callback propagates from `test_graph`. Each test run constructs and
destroys a new runtime graph.

An input view exposes `valid`, `modified`, `last_modified_time`, `value`, and
`delta_value`. Change queries follow the [node input API](node-api.md#complete-time-series-access).
`last_modified_time` is a Python `datetime` mapped from the engine update time.
Input views and their lazy values, change views, and
iterators expire when the callback returns.

`TSS` values are set-like. `added()` and `removed()` return lazy set-like
views, and `was_added(value)` and `was_removed(value)` test the current
change. A fixed `TSL` provides child input iteration. Fixed `TSL` and `TSB`
inputs provide child access through `keys()`, `values()`, `items()`,
`modified_*()`, and `valid_*()` methods. `TSB` fields also support attribute
access. `TSD` inputs provide mapping access, `key_set`, and separate
`added_*()`, `removed_*()`, `modified_*()`, and `valid_*()` methods. Collection
`value` and `delta_value` are lazy set, sequence, or mapping views according to
their time-series shape. Use `copy()`, `list(...)`, `set(...)`, or `dict(...)`
to materialize one.

Output deltas use these forms:

| Output | Delta |
| --- | --- |
| `TS[T]` | a `T` value |
| `REF[TS_T]` | `ReferenceValue` obtained from a reference input |
| `TSS[T]` | a `set` or `frozenset` of additions and `Removed(value)` markers, or `SetDelta(...)` |
| fixed `TSL[TS_T, N]` | `{index: item_delta}` |
| `TSB[Schema]` | `{field_name: item_delta}` |
| `TSD[K, TS_T]` | `{key: item_delta}`; use `REMOVE` as an item delta to remove a key |

`InputView.delta_value` uses the same mapping form for TSD changes. A removed
key maps to `REMOVE`; added and modified keys map to their item deltas. A TSS
input delta has lazy `added` and `removed` views. Collection item deltas recur
through the same table.

The built-in scalar annotations are `bool`, `int`, `float`, `str`,
`datetime.timedelta`, and `bg.Symbol`. Durations map to signed 64-bit
microseconds; conversion rejects values outside that range.
`list[T]` declares an engine scalar list. Conversion copies its items using
the declared scalar type `T`; conversion back to Python produces a list.
`bg.Symbol` is a `str` subclass. Non-null symbols compare and
hash like their text; `bg.Symbol()` represents a null symbol and differs from
the empty string. `str(symbol)` returns ordinary Python text, so converting a
null symbol produces `""`. `bg.Symbol` cannot be subclassed. Python `int` maps
to the engine's signed 64-bit integer type.
Python `str` maps to `bg::string` and supports scalar arguments and `TS[str]`
inputs and outputs. Conversion uses strict UTF-8: lone surrogates raise
`UnicodeEncodeError`, and invalid UTF-8 from C++ raises `UnicodeDecodeError`.
C++ callers must provide valid UTF-8. For canonical strings whose storage has
no other final-release hook, the extension retains the originating Python
string object while the BG storage lives and removes the cache entry when that
storage is released. `bg.Symbol` values infer the engine symbol type, while ordinary
`str` values infer the engine string type. `bg.symbol` is an alias for the
`bg.Symbol` constructor. A `Symbol` supplied to a `str`-typed input is
normalized to ordinary Python text; a plain `str` supplied to a `Symbol`-typed
input is interned as a non-null symbol. The Python class reuses objects
produced from the same interned engine symbol while retaining the class-level
cache.

`tuple[T, ...]` declares the homogeneous scalar list `bg::List<T>`.
`typing.Tuple[T, ...]` declares the same type. The item annotation can contain
scalar type variables or nested scalar collections. Items require scalar types;
optional items are unsupported. For example:

```python
@bg.node
def total(values: tuple[int, ...]) -> int:
    return sum(values)
```

Fixed tuple annotations declare `bg::Tuple` with one scalar type per position.
Their item annotations can contain scalar type variables or nested scalar
collections. Items require scalar types; optional items are unsupported. Fixed
tuples support heterogeneous types, including the one-item form `tuple[T]` and
the empty form `tuple[()]`. `typing.Tuple` supports the same forms. For example:

```python
@bg.node
def describe(value: tuple[str, int]) -> str:
    return f"{value[0]}: {value[1]}"
```

List and fixed tuple values materialize as immutable Python tuples in node
arguments and graph results. When the expected scalar type is known, conversion
accepts Python tuples and lists, converts each item through its scalar
conversion, and owns the engine storage. Later mutations of the source Python
list do not change the stored value.

`list[T]` and `typing.List[T]` are unsupported annotations because they describe
mutable sequences. Bare `list` and `tuple` annotations are unsupported. Fixed
tuple conversion requires the declared number of items and converts each
position using its declared scalar type.

Other Python classes can be used as scalar annotations and values. The first
use registers one semantic scalar type for that exact class. Python inheritance
defines scalar compatibility, including multiple inheritance; a derived value
retains its actual Python object when stored through a base annotation. Existing
built-in mappings and explicit extension registrations take precedence during
type inference.
Python `None` remains a sentinel for no output change and skipped test-input
updates, not a Python-object scalar value.

Python object scalars retain their Python object by reference. Copying and
releasing an existing scalar handle does not acquire the Global Interpreter
Lock (GIL). A final Python reference is released immediately when the GIL is
held; otherwise, release is deferred until the next periodic Python callback or
graph-run boundary.
Python `__hash__` and `__eq__` run with the GIL and their exceptions propagate.
Python objects used as graph scalars must be immutable, including any state
that affects equality or hashing.

### Queue nodes

`@bg.queue` declares a queue node whose decorated function runs at graph start.
Its return annotation declares the output time series type; its return value is
ignored. The function declares exactly one `q: bg.Queue = None` parameter.
Graph code supplies its scalar parameters. A queue node has no evaluation
callback and runs in the outer graph. Register cleanup with `@name.stop`,
selecting parameters by name as for other Python nodes.
Queue callbacks can also request `State[T]` and `EvaluationClock`.

```python
@bg.queue(mode=bg.QueueMode.batch)
def values(count: int, q: bg.Queue = None) -> bg.TS[tuple[int, ...]]:
    for value in range(count):
        q.write(value)

@bg.graph
def example() -> bg.TS[tuple[int, ...]]:
    return values(3)

assert bg.test_graph(example) == [(0, 1, 2)]
```

The default mode is `bg.QueueMode.standard`. `bg.QueueMode.elide` and
`bg.QueueMode.batch` follow the [queue message rules](time-series-types.md#queue-messages).
Queue outputs use ordinary time series. Batch outputs have `TS[tuple[T, ...]]`
leaves, including leaves inside fixed `TSL`, `TSB`, and `TSD` collections.

`Queue.write(delta)` returns whether it committed a message. It accepts the
existing Python output delta forms. `None` requests no message and returns
`False`. Batch mode accepts one `T` for each updated `TS[tuple[T, ...]]` leaf.
Invalidation uses `bg.INVALID`; dictionary removal uses `bg.REMOVE`. An open
writer propagates conversion and serialization errors without committing a
partial message.

Retain a `Queue` after its callback returns to submit later messages. Use
`copy.copy(q)` to create an independent writer for another thread. A handle
rejects overlapping writes and copying during a write. Graph shutdown closes
all writers before stop callbacks; closed writes return `False` without
converting the delta. Retained handles remain safe after graph destruction.
Use [realtime execution](#graph-authoring) to wait for messages from producers.

## C++ extensions

An extension can expose a process-lifetime operator without adding it to a
central registry:

```cpp
#include <bg/operators.hpp>
#include <bg/python/operator.hpp>

bg::python::export_operator(module, "getitem", bg::operators::getitem);
```

The wrapper observes overloads registered on that operator later. It uses the
same resolution cache, requirements, resolvers, adaptations, and diagnostics
as a C++ call.

`PythonScalarConversions::add_python_type` associates a Python class with a
scalar type. Lookup follows the Python method resolution order. Register the
corresponding Python-to-scalar conversion before values of a new scalar type
are wired. Registrations retain their Python classes and last for the process.
Extensions can register Python-to-scalar and scalar-to-Python handlers with
`scalar_conversions().add(...)`. Scalar-to-Python-view handlers provide
callback-scoped representations for direct `@node` arguments. Without one,
view conversion uses the ordinary scalar-to-Python conversion. Runtime-specific
handlers take precedence over handlers for the scalar type. Register handlers
when the extension module initializes, before affected values are exposed to
Python.

Native node declarations use `bg::python::PythonObject` to constrain a scalar
variable to Python-object values. User-defined Python classes represented by
Python-object scalars match the root `object` bound:

```cpp
using PYOBJECT = bg::ScalarVariable<"PYOBJECT", bg::python::PythonObject>;
bg::In<bg::TS<PYOBJECT>> input;
```

Python-to-scalar conversion during wiring receives a temporary allocator. A
conversion whose result can retain shared storage beyond wiring must allocate
that storage from a longer-lived owner or the current runtime allocation context.

Python wiring points are temporary proxies used while a graph is being
constructed. They expire when graph construction returns.

## Higher-order graphs

`map`, `mesh`, and `switch_` accept decorated Python graph definitions and use
the engine's normal C++ implementations.

```python
@bg.graph
def branch(key: bg.TS[int], value: bg.TS[float]) -> bg.TS[float]:
    return scale(value)

mapped = bg.map(branch, values, __key_arg__="key")
```

Map and mesh accept positional and named arguments. `pass_through(value)`
passes a complete time series to every branch. Their key and key-set selectors
use `__key_arg__` and `__key_set_arg__`.

`mesh()[key]` selects a branch from the current mesh.
`mesh("name")[key]` selects a named enclosing mesh. A mesh definition accepts
`name="name"`.

Switch cases use a dictionary. A tuple groups keys that use the same graph, and
`DEFAULT` supplies the fallback graph:

```python
selected = bg.switch_(key, {(1, 2): first, 3: second, bg.DEFAULT: fallback}, value)
```

Source and sink branch graphs are supported. A sink higher-order call returns
`None`.

## Runtime boundary

The `bg` engine does not depend on Python, nanobind, Python object ownership,
the Global Interpreter Lock (GIL), or Python exception types. The Python runner
releases the GIL while native graph evaluation runs. Each Python node callback
acquires it for argument conversion, callback execution, and output
conversion.

C++ nodes that call Python APIs use the Python wrapper: `bg::python::node<Definition>()`
for aggregate declarations, `bg::python::node(callable)` for callable nodes, and
`bg::python::node(definition)` for runtime-signature nodes. The wrapper acquires
the GIL for evaluation, start, invalidation, stop, and finalization callbacks.
Exceptions from evaluation, start, and invalidation propagate through graph
execution. Since stop and finalization are `noexcept` engine callbacks,
pending Python exceptions from them are reported through `sys.unraisablehook`
after cleanup. Runtime-signature definitions wrapped this way must not be
declared `final`.

The current package does not expose dynamic Python node inputs.

Python definition wrappers retain their callbacks. A wired runtime graph also
retains each Python node callback that it uses. Destroy these graphs before
shutting down the Python interpreter.

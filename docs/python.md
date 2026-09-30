# Python integration

The Python package provides initial graph authoring and node callbacks while
keeping Python and nanobind out of the `bg` engine target. Build it with
`BG_BUILD_PYTHON=ON`. The installed CMake package then provides `bg::python` in
addition to `bg::bg`.

## Build and install

The Python extension requires Python 3.10 or newer, its development module, and
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

`bg.operators` exposes engine operators through their normal C++ overload
resolution. Positional and named arguments can contain wiring points or
registered Python scalar values. A sink operator returns `None`.

```python
@bg.graph
def constant() -> bg.TS[int]:
    return bg.operators.const_(value=42)

item = collection[key]  # bg.operators.getitem(collection, key)
```

Node and graph functions accept positional and named arguments according to
their Python signatures, including positional-only, keyword-only, `*args`, and
`**kwargs` parameters. A graph function expands into the active graph while it
is being wired; it does not create a runtime nested-graph boundary. Scalar
arguments are supported. `Optional[T]` is supported for scalar parameters and
passes `None` to the callback when absent.

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

`test_graph` accepts one update sequence for every graph parameter, and every
graph parameter must currently be `TS[T]`. Each sequence element is a value or
`None`; `None` skips that input's update for the cycle. The graph must return a
`TS[T]`. The result has one entry per cycle: an output value when the graph
publishes one, otherwise `None`.

## Time-series types

Python definitions can use these annotations:

```python
bg.TS[int]
bg.REF[bg.TS[int]]
bg.TSS[int]
bg.TSL[bg.TS[int], 4]
bg.TSD[int, bg.TS[float]]
bg.TSB[Quote]
```

A non-variadic TSL requires a fixed size. A TSB uses a Python schema:

```python
class BaseQuote(bg.TimeSeriesSchema):
    bid: bg.TS[float]

class Quote(BaseQuote):
    ask: bg.TS[float]
```

Schemas support single inheritance. Each schema class receives a distinct
native identity; its qualified name is descriptive.

## Python nodes

`@source_node` declares a no-input node that runs in the first evaluation
cycle. `@compute_node` declares a fixed-input node. Inputs can be any supported
time-series type or a scalar argument.

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

`@node` parameters must use scalar annotations, not `TS[...]`. `Optional[T]`
marks a scalar argument as optional and makes a time-series input optional if
that parameter is wired to a time series.

A callback returns an output delta. Returning `None` requests no output change.
Sink nodes have no output annotation and must return `None`. An exception
raised by a callback propagates from `test_graph`. Each test run constructs and
destroys a new runtime graph.

An input view exposes `valid`, `modified`, `value`, and `delta_value`.
Collection inputs also support item access with `value[key]`. Item access
returns another input view. All input views expire when the callback returns.

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
key maps to `REMOVE`; added and modified keys map to their item deltas.
TSS input `delta_value` remains a `SetDelta`. Collection item deltas recurse
through the same table.

The currently accepted scalar annotations are `bool`, `int`, `float`, `str`,
and `bg.Symbol`. Python node outputs currently support `bool`, `int`, `float`,
and `bg.Symbol`; string time-series outputs are deferred. Python `int` maps to
the engine's signed 64-bit integer type. `str` is supported as a scalar node
argument. `bg.Symbol` is distinct from `str`; `bg.symbol` constructs one. A
null `bg.Symbol()` differs from `bg.Symbol("")`.

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

The current package does not expose Python-object scalar storage, scalar bundle
schemas, injected runtime services, or dynamic Python node inputs. Direct
`TS[str]` output storage is also deferred; strings are supported as scalar
wiring arguments.

Python definition wrappers retain their callbacks. A wired runtime graph also
retains each Python node callback that it uses. Destroy these graphs before
shutting down the Python interpreter.

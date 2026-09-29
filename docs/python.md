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

The package exports `TS[T]`, `@source_node`, `@compute_node`, `@graph`,
`test_graph`, `Symbol`, and `symbol`.

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

Node and graph functions accept positional and named arguments according to
their Python signatures. Parameters must be positional-only or
positional-or-keyword. A graph function expands into the active graph while it
is being wired; it does not create a runtime nested-graph boundary. Scalar
arguments are supported. `Optional[T]` is supported for scalar parameters and
passes `None` to the callback when absent.

`test_graph` accepts one update sequence for every graph parameter, and every
graph parameter must currently be `TS[T]`. Each sequence element is a value or
`None`; `None` skips that input's update for the cycle. The result has one entry
per cycle: an output value when the graph publishes one, otherwise `None`.

## Python nodes

`@source_node` declares a no-input node that runs in the first evaluation
cycle. `@compute_node` declares a fixed-input node. Inputs can be `TS[T]`
views or scalar arguments. A time-series input view exposes `valid`,
`modified`, and `value`; it becomes invalid when the callback returns.

A callback's return value is a scalar output delta. Returning `None` requests
no output change. Sink nodes have no output annotation and must return `None`.
An exception raised by a callback propagates from `test_graph`. Each test run
constructs and destroys a new runtime graph.

The currently accepted scalar annotations are `bool`, `int`, `float`, `str`,
and `bg.Symbol`. Python node outputs currently support `bool`, `int`, `float`,
and `bg.Symbol`; string time-series outputs are deferred. Python `int` maps to
the engine's signed 64-bit integer type. `str` is supported as a scalar node
argument. `bg.Symbol` is distinct from `str`; `bg.symbol` constructs one. A
null `bg.Symbol()` differs from `bg.Symbol("")`.

Python wiring points are temporary proxies used while a graph is being
constructed. They expire when graph construction returns. Input views expire
when their callback returns.

## Runtime boundary

The `bg` engine does not depend on Python, nanobind, Python object ownership,
the Global Interpreter Lock (GIL), or Python exception types. The Python runner
releases the GIL while native graph evaluation runs. Each Python node callback
acquires it for argument conversion, callback execution, and output
conversion.

The current package does not expose Python operators on wiring points,
collection annotations and deltas, reference annotations, Python-object scalar
storage, Python-declared schemas, injected runtime services, or higher-order
graph operators. These are not part of the implemented Python API.

Python definition wrappers retain their callbacks. A wired runtime graph also
retains each Python node callback that it uses. Destroy these graphs before
shutting down the Python interpreter.

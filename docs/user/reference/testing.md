# Inputs and captured results

`run_graph` captures sparse output events by default. A captured event is a publication
or invalidation at an evaluation time. A skipped position can still have a retained
current graph value.

## Input sequences

Python uses a sequence for each changing graph input. C++ literal sequences infer types
from concrete receiving parameters. `input<TS<T>>({...})` supplies an explicit type
when the declaration is unresolved. Generic Python definitions can use concrete
specialization such as `definition[TS[int]]`.

`input(time_series_type, {...})` accepts a semantic time series type selected at
runtime. Literal sources use that type's default runtime representation, including
for empty and skip-only inputs.

`None` in Python and `SKIP` in C++ supply no update. `INVALID` invalidates a value.
Collection deltas update supplied items and retain omitted items. `REMOVE` removes a
dictionary key. [Literal forms](literals.md) specify supported values.

Input positions are one microsecond apart, starting at the resolved run start.
Scheduled work can extend capture beyond the input sequences. Both execution engines
support capture. See [execution](execution.md) for fixed arguments, `ticks`, limits,
and engine selection.

## Python results

`GraphRunResult` exposes:

| API | Result |
| --- | --- |
| `.start_time` | Origin for dense position offsets |
| `.events` | Sparse `(time, value)` event tuples |
| `.event_count` | Number of captured events |
| `len(result)` | Dense microsecond position count |
| `result[index]` | Value at one position, or `None` |
| `result[start:stop]` | Dense slice as a list |
| `list(result)` | Complete dense list |
| `result == expected_list` | Dense update comparison |

Dense access fills empty positions with `None`; root invalidation also materializes
as `None`. Observe `valid` when a test must distinguish them. Prefer sparse events
for large time gaps.

## C++ results

`GraphRunResult<T>` exposes `start_time()`, `size()`, `event_count()`, and `events()`.
An untyped `GraphRunResult<>` supports literal `equals({expected...})`, including
`SKIP` and `INVALID`. A typed `GraphRunResult<T>` compares scalar values and
`std::nullopt` positions with `equals`. A typed empty position does not distinguish
no update from invalidation.
Scalar indexing returns an owning optional scalar value. `view()` provides scalar
access when a value exists.

`visit_event(index, visitor)` reads a sparse event with its time. `visit_tick(index,
visitor)` reads one dense position without copying payload values and returns false
for a skipped position. Visitor views expire when the callback returns.
Captured data remains owned by the result.

## Printing results

Python `print(result)` and C++ `std::cout << result` display results with at most
32 positions as lists. A skipped position or root invalidation prints `None`.
Larger results display `GraphRunResult(start_time=..., event_count=..., size=...)`
without expanding the gaps between events. Printing preserves captured values.

C++ values use [tick formatting](library-text.md#formatting-and-printing): booleans print
as `true` and `false`, strings use double quotes, and floating-point values can
omit `.0`. Collection outputs display changed items, including nested `INVALID`
and `REMOVE` markers. Python uses its value representations.

## Test coverage

A meaningful test selects the behavior it needs to distinguish:

- Repeated equal values and skipped updates.
- Invalidation and recovery.
- Collection addition, modification, invalid items, removal, and re-addition.
- Fresh state in separate runs and independent graph instances.
- Simultaneous inputs and scheduled work at boundaries.

[Testing](../how-to/testing.md) gives a complete example with assertions.

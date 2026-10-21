# Test graphs

`test_graph` constructs a fresh graph, feeds input updates in simulation, and
records output updates. Use it to check values, timing, skipped updates,
invalidation, and collection changes without external producers.

## Calls

| Language | Form |
| --- | --- |
| Python | `test_graph(definition, *sequences, catalogue=None, recorder=None, engine=EvaluationEngine.simulation, start_time=None, stop_time=None, run_time=None)` |
| C++ | `test_graph(function, sequences...)` or `test_graph(function, GraphRunOptions{...}, sequences...)` |

Python requires a decorated graph. Each sequence corresponds to a graph input
and uses its declared type. Plain graph scalar annotations accept this changing
input form. A generic definition needs concrete input specialization.
Scalar-only parameters must be bound during construction through a wrapper graph.

C++ accepts ordinary graph functions, graph templates, and lambdas. Literal
sequences infer types from the receiving parameter. `input<TS<T>>({...})`
supplies a type if the declaration leaves it unresolved. Ordinary C++ range
inputs are also accepted; the guide uses literal sequences to expose update
behavior directly.

## Time positions

Input positions are one microsecond apart, starting at the run's start time.
Sequences can have different lengths; after an input's last position it supplies
no further updates. Current input values remain available. Scheduled work can
extend the output recording. A graph with no changing input still has its start
cycle. Run limits exclude work at and beyond the stop boundary.

Default simulation starts at epoch plus one microsecond. Python custom start
times are timezone-naive local `datetime` values. C++ uses `EvaluationTime`.
`test_graph` requires the simulation engine.

## Results

Python returns one recorded position per test time step. C++ returns a
`TestGraphResult`; `.equals` compares expected updates. Results use
[literal delta forms](literals.md), not a cumulative value at every position.
A skipped output position can still have a retained current value.

Test fresh state with separate runs. Test repeated equal inputs separately from
skips. Include invalidation and recovery when a node's contract depends on them.
For collection operations, check additions, invalid items, removals, and re-additions.
See [testing a calculation](../how-to/testing.md) for a complete example.

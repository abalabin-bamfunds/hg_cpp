# Node declarations

A node performs one calculation or side effect when BG evaluates it. A required input
needs a current value. An active input schedules evaluation when it changes. Inputs are
active and required unless declared otherwise.

## Python declarations

| Declaration | Parameters | Result | Initial evaluation |
| --- | --- | --- | --- |
| `@node` | Plain scalar annotations; fixed scalars or current `TS` values | Plain scalar return or no output | When all resolved arguments are scalars |
| `@compute_node` | Explicit time series views and fixed scalar arguments | Explicit time series delta or no output | Input driven |
| `@source_node` | Scalar arguments and permitted injectables | Explicit time series delta | At graph start |
| `@generator` | Scalar arguments, clock, or state | Yields timed deltas | First source invocation at graph start |
| `@queue` | Scalar arguments, one `Queue`, optional state or clock | Output type annotation; callback return ignored | Start callback writes messages |

A plain `int` parameter on `@compute_node` is scalar-only. Use `TS[int]` to receive a
changing input view. `@node` parameters cannot use time series annotations. `Scalar[T]`
restricts a plain parameter to a scalar.

`int | None` on a plain node input permits an absent value. A plain node declares
its output as `-> int` and may return `None` to leave that output unchanged; an optional return annotation is not
supported.

A callback returning a valid scalar publishes an update even if equal to the previous
value, unless the output's guarantee suppresses it. Returning `INVALID` removes a
supported output's current value. Collection results are deltas; see
[literals](literals.md).

### Input activity and validity

`@node` and `@compute_node` accept tuples of input names:

| Option | Selected inputs | Other time series inputs |
| --- | --- | --- |
| `active=("request",)` | Schedule evaluation on updates | Passive |
| `valid=("reading",)` | Require a current value | Optional, unless selected by `all_valid` |
| `all_valid=("items",)` | Require the root and every immediate item to be valid | Follow `valid` or the default |

Omitting `active` makes every time series input active. Omitting `valid` keeps
annotation-derived validity: ordinary views are required, plain optional inputs
permit absence. An explicit `valid` tuple replaces that selection. `all_valid`
adds the stricter constraint to its selected collection inputs. A name cannot
appear in both `valid` and `all_valid`. An empty tuple selects no inputs for
that option.

These options apply to parameters resolved as time series inputs. Fixed scalars
are unchanged. Names must identify time-series-capable parameters; fixed scalar
and injected parameters cannot be selected. Optional compute views still use
`TS[...]` annotations; check `.valid` before `.value`. Plain optional inputs
receive `None` when absent. See [validity checks](../guide/advanced-nodes/inputs.md#optional-views).

Activity and validity are independent. A passive required input still gates
evaluation; an active optional input can schedule evaluation while absent.
Use [`sample`](library-selection.md#sample) when a library operation already
provides the required request-driven read.

## C++ declarations

`node(callable)` infers a node from a callable. Plain scalar parameters receive fixed
arguments or current input values. `std::optional<T>` permits absence. A scalar return
declares `TS<T>`; `std::optional<T>` returns no update when disengaged. A `void` return
declares no output unless an `Out<>` parameter supplies it.

`node<Definition>()` uses an aggregate with input, scalar, state, and output fields. Its
`evaluate()` method is `const`. It can define `start`, `stop`, and `on_invalidation`
callbacks. Fields such as `In<TS<i64>>` and `Out<TS<i64>>` provide access during
callbacks. Aggregate callbacks can receive `EvaluationContext*`; callable node
parameters do not currently expose it.

`In<TS_T, ...>` requests a complete input view. Properties include `optional`,
`all_valid`, `passive`, and a parameter name. `passive` suppresses scheduling by that
input; it does not relax its validity requirement. `all_valid` requires the root and all
immediate contained inputs to be valid. `make_active()` and `make_passive()` adjust a
dynamic input's subscriptions during evaluation where supported by its declaration.
Python selects these constraints with the decorator options above.

## Scheduling and validity

An input update schedules at most one evaluation in the outer cycle. Evaluation reads
dependency results from that cycle. All required input constraints must hold before the
callback runs. A node can also be scheduled at start or by a timer. Startup scheduling
does not bypass required inputs.

C++ `schedule_on_start` selects initial evaluation explicitly. Aggregate sources with no
inputs need `static constexpr bool schedule_on_start = true` when they rely on initial
evaluation rather than a start callback or timer.

Required-input invalidation normally invalidates the output without evaluating the
ordinary callback. C++ `on_invalidation = preserve_output` keeps it; a custom
invalidation callback can implement another policy. Python registers
`@definition.on_invalidation`; it runs when required-input invalidation prevents
ordinary evaluation. Optional inputs permit the ordinary callback to handle absence
itself. See [output access](outputs.md) and [state](state.md).

Exceptions propagate from construction or evaluation to the runner after graph cleanup.
Stop cleanup must not throw; Python reports stop failures through the
unraisable-exception hook. Error context identifies affected graph calls.

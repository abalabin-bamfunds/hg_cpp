# Node declarations

When a graph needs application logic or an action such as sending a result, declare a
node. HG calls it during evaluation with its current inputs. Inputs are active and
required by default: changes schedule evaluation, and each input needs a current value
before the callback can run.

## Python declarations

Use `@node` for ordinary value code, `@compute_node` when the calculation needs input
views, or a source declaration when the node introduces its own events:

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
value, unless the output's guarantee suppresses it. Python callbacks currently cannot
return `INVALID` to invalidate an output. Collection results are deltas; see
[output publication](outputs.md#returned-values) and [literals](literals.md).

### Input activity and validity

If a node should run only on selected updates or continue while some inputs are
unavailable, set its activity and validity options. Both `@node` and `@compute_node`
accept tuples of input names:

| Option | Selected inputs | Other time series inputs |
| --- | --- | --- |
| `active=("request",)` | Schedule evaluation on updates | Passive |
| `valid=("reading",)` | Require a current value | Optional, unless selected by `all_valid` |
| `all_valid=("items",)` | Require the root and every immediate item to be valid | Follow `valid` or the default |

The [input validity and activity guide](../guide/advanced-nodes/inputs.md) explains
when to choose required, optional, or `all_valid`, and how each combines with active
or passive inputs. Its [request-driven example](../guide/advanced-nodes/inputs.md#active-and-passive-inputs)
compares required and optional passive readings.

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

To connect an existing C++ calculation, pass its callable to `node(callable)`, which
infers the node declaration. Plain scalar parameters receive fixed arguments or
current input values, and `std::optional<T>` permits absence. A scalar return declares
`TS<T>`; `std::optional<T>` returns no update when disengaged. A `void` return declares
no output unless an `Out<>` parameter supplies it.

For a node whose inputs, state, output, and lifecycle are clearer as one definition,
use `node<Definition>()` with an aggregate. Its `evaluate()` method is `const`, and it
can define `start`, `stop`, and `on_invalidation` callbacks. Fields such as
`In<TS<i64>>` and `Out<TS<i64>>` provide access during callbacks. Aggregate callbacks
can receive `EvaluationContext*`; callable node parameters do not currently expose it.

To set either control on a C++ input view, add its property to `In<TS_T, ...>`:

| Control | Declaration | Contract |
| --- | --- | --- |
| Required | `In<TS<i64>, required>` | Require root validity; this is the default |
| Optional | `In<TS<i64>, optional>` | Permit an invalid input |
| Complete collection | `In<TSL<TS<i64>, 2>, hg::all_valid>` | Require root and immediate-item validity |
| Active | `In<TS<i64>, active>` | Input changes schedule evaluation; this is the default |
| Passive | `In<TS<i64>, passive>` | Input changes do not schedule evaluation |

Combine one validity property and one activity property, for example
`In<TS<i64>, optional, passive>`. Property order has no meaning. `all_valid` checks
immediate items rather than every descendant of a nested collection. A parameter
name is independent of these controls. Python selects the same constraints with the
decorator options above.

For C++ inputs declared with `dynamic_activity`, `make_active()` and
`make_passive()` adjust activity during callbacks without changing the validity
constraint. See [input access](input-views.md).

## Scheduling and validity

When a calculation combines inputs that change together, HG schedules at most one
evaluation in the outer cycle and supplies the dependency results from that cycle.
All required input constraints must hold before the callback runs. A node can also be
scheduled at start or by a timer; startup scheduling does not bypass required inputs.

C++ `schedule_on_start` selects initial evaluation explicitly. Aggregate sources with no
inputs need `static constexpr bool schedule_on_start = true` when they rely on initial
evaluation rather than a start callback or timer.

If losing a required input needs a policy such as retaining the last result, use an
invalidation callback. Required-input invalidation normally invalidates the output
without evaluating the ordinary callback. C++ `on_invalidation = preserve_output`
keeps it; a custom invalidation callback can implement another policy. Python registers
`@definition.on_invalidation`; it runs when required-input invalidation prevents
ordinary evaluation. Optional inputs permit the ordinary callback to handle absence
itself. See [output access](outputs.md) and [state](state.md).

Exceptions propagate from construction or evaluation to the runner after graph cleanup.
Stop cleanup must not throw; Python reports stop failures through the
unraisable-exception hook. Error context identifies affected graph calls.

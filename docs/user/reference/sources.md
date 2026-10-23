# Sources, timers, and queues

Sources introduce updates without changing graph inputs. Use library `const_` and
`schedule` for fixed values and periodic signals. Custom sources supply
application-specific values or external events.

## const_

`const_(value)` creates a time series whose scalar type is inferred from `value`. It
publishes once at the start when observable and never changes. It accepts registered
scalar values, including scalar collections; it does not split them into independently
updating items. Python imports it from `bg.operators`; C++ uses `bg::operators::const_`
from `<bg/library.hpp>`.

## nothing

`nothing[TS[int]]()` creates a series that never becomes valid. It has the final
guarantee. Required inputs connected to it remain blocked; optional inputs can
evaluate from another active input and observe `valid == False`. It produces no
updates and compiles without a runtime producer node.

Python imports `nothing` from `bg.operators`. C++ supplies the time-series type
explicitly:

```cpp
auto missing = operators::nothing[type<TS<i64>>]();
```

The type argument uses normal time-series type-variable matching. A reference
type argument matches through its target, so `nothing[REF[TS[int]]]()` creates
an invalid target series. A reference input can receive a valid reference to
that invalid series.

## Callback sources

Python `@source_node` evaluates once at graph start. It accepts scalar arguments and an
explicit time series output annotation. It can request `Scheduler` to schedule later
evaluations. A plain `@node` with only scalar arguments also runs at start. C++
aggregate sources use a start callback, startup evaluation, or explicit scheduling.

Python injectables have `= None` defaults and are absent from graph-call arguments.
`EvaluationClock` exposes `evaluation_time` and `now`. `Scheduler` provides
`schedule_at`, `schedule_wall_clock_at`, `cancel`, and `timer_fired`. Clock and
scheduler objects expire after the callback. A schedule handle can be retained. A node
may schedule only if it declares scheduling support; requesting a clock alone does not
enable it.

C++ aggregates declare `static constexpr bool uses_timed_scheduling = true` and use
`EvaluationContext` timer methods. Timer activation schedules the owning node; input
validity constraints still apply. Pending timers belong to that node and stop with its
graph.

## Python generators

`@generator` yields `(datetime, delta)` for absolute evaluation time or `(timedelta,
delta)` relative to current evaluation time. Times increase strictly. An empty generator
is valid. Exhaustion ends its events; graph shutdown closes its frame. Generators accept
clock and state, but cannot request `Scheduler` or `Output`.

## Queues

Python `@queue` declares a start callback with exactly one `q: Queue = None` and an
explicit time series output. Its return is ignored. C++ `queue<OutputType>` constructs a
queue source from a start callable taking `Queue`. Use queue handles to submit messages
from external producers.

`write(delta)` returns whether a message committed. A no-update delta commits nothing.
Conversion or serialization failure leaves no partial message. Closed handles return
false before inspecting the delta. Shutdown closes handles before stop callbacks.
Handles remain safe after graph destruction.

Each handle has one writer cursor. Use a separate copy for each producer; do not copy or
overlap writes through one handle during a write. Python uses `copy.copy(q)`; C++ copies
`Queue`. Retaining a queue handle beyond its callback is supported, unlike retaining an
input view.

| Mode | Effect |
| --- | --- |
| standard | Preserves individual messages for delivery |
| elide | Combines pending messages into the latest state |
| batch | Delivers a group of scalar messages as one scalar list at each leaf |

Python selects `QueueMode.standard`, `.elide`, or `.batch` on `@queue(mode=...)`. C++
uses `QueueMode::standard`, `::elide`, or `::batch`. Batch outputs declare list leaves,
`TS[tuple[T, ...]]` in Python or `TS<List<T>>` in C++, but writers submit individual `T`
leaf values. Realtime execution waits for producers; simulation can finish when no
pending work remains.

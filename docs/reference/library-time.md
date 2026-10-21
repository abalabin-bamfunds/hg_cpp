# Time operators

Timed operators use evaluation time. C++ durations use `timedelta_t`; Python uses `datetime.timedelta`. Arguments such as a period are fixed during construction.

## delay

`delay(value, duration)` publishes each input event at its original evaluation
time plus `duration`. It retains ticks, including invalidation and collection
deltas, rather than sampling the value later. Equal ordinary ticks remain
separate events. Zero duration returns the input; negative duration is a wiring
error. Positive delay limits const guarantees to final, including contained
time series. Retained ticks follow the supported types and ownership rules in
[retaining ticks](library-time.md#retained-events). Delay rejects contained
references and sparse TSL values. TSS and TSD root invalidation cannot be
replayed, so retargeting from a valid TSS or TSD to an invalid target fails.
Initially unbound references are supported.

## schedule

`schedule(period, initial_delay=true, max_ticks=...)` publishes ordinary
`TS<bool>` ticks whose value is true. The first tick occurs one period after
graph start. `initial_delay=false` produces the first tick at graph start.
`max_ticks` limits the number of ticks; zero produces none. Without a limit,
the source continues until its graph stops. Period must be positive and
`max_ticks` must be nonnegative.

## throttle

`throttle(value, period, delay_first_tick=false)` passes the first event
immediately and starts a period. Events during that period coalesce. At its
end, throttle publishes the latest pending state and starts another period.
With no pending event, it stops scheduling until the input changes again.
`delay_first_tick=true` also delays the first event by one period. Period must
be positive.

Throttle applies the same timing to invalidation. An input change at the
publication time contributes to that publication. Collection publication
reconciles the complete current value, so changes that cancel within the
period need not produce an output tick. The output retains input guarantees up
to final. Throttle retains timing state, not a copy of the input value.

Durations are scalar arguments: use `bg::timedelta_t` in C++ and
`datetime.timedelta` in Python. Timed work uses evaluation time and stops with
the owning graph, including inner graphs. Scheduling outside the supported
[evaluation-time range](execution.md) fails.


## Retained events

Delay retains owning tick data for supported scalar, set, dictionary, bundle,
and fixed-list shapes. It rejects contained references and sparse TSL values.
A retained scalar refers to its owned value at the input event, not the
source's later current value. Throttle retains timing state and reads the
current source at publication, reconciling collection state then.

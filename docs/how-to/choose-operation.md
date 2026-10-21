# Choose an update operation

Use the required output behavior to select an operation. Equal values and
separate update events are distinct inputs to this decision.

| Need | Operation | Result behavior |
| --- | --- | --- |
| Publish only value changes | `dedup` | Suppresses equal requested values |
| Forward while enabled | `filter_` | Closed output keeps its last value; reopening can forward current input |
| Read on each request | `sample` | Signal update samples current value, including a false signal |
| Select a current value | `if_then_else` | Follows one input; both computations remain constructed |
| Select a running computation | `switch_` | Starts and stops case instances |
| Preserve each event for later | `delay` | Replays each retained event after a duration |
| Limit frequency and retain latest state | `throttle` | Combines pending events within a period |
| Count events | `count` | Counts valid root updates |
| Read the previous event | `lag` | Advances once per input event |
| Total over successive events | `sums` | Repeated readings contribute again |
| Total over current collection items | `sum_` | Replaces contributions as items change |
| One calculation per dynamic key | `map` | Independent branch state and lifecycle |
| Per-key calculations with dependencies | `mesh` | Dependency branches evaluate first |

Filtering, sampling, and deduplication before a running calculation change which
updates contribute. Placing deduplication after it only suppresses equal result
publications. A time series itself stores its current value; historical
operations retain their own state.

See the [library index](../reference/library-index.md) for complete contracts.

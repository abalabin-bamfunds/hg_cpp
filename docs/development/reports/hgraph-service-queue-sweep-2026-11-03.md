# hgraph service queue sweep, 2026-11-03

This batch checks queue sources owned by service implementations. The upstream
pattern is from hgraph 0.8 at commit [`d36be500ec8b6fda77ff077566f891ffcf5c84a8`][upstream],
in `tests/cpp/test_service_push_sources.cpp`. HG is based on `53194f8d6` for this batch.

## Adaptation

HG queue nodes provide queue-backed time series to graph code. Each case
registers an implementation graph that returns a queue node's output. Queue
writes happen at graph start or in a node, so the simulation traces are
deterministic. The queue implementation tests in
[`runtime_graph_queue_test.cpp`](../../../tests/evaluation/time_series/runtime_graph_queue_test.cpp)
already cover message scheduling and queue mechanics; this batch checks queue
sources through service boundaries and higher-level graphs.

The subscription cases use a final key source, as required by HG's
[service contract](../design/services.md#service-declarations). Request/reply
cases write responses to a queue using the service client IDs carried in the
request dictionary.

## Added coverage

The cases are in
[`service_queue_patterns_test.cpp`](../../../tests/evaluation/service_queue_patterns_test.cpp).

| Pattern | Behavior asserted |
| --- | --- |
| Reference service | Queue values arrive in separate simulation cycles; the queue closes after the run. |
| Shared clients | Two clients use one implementation, which starts once. |
| Separate implementations | Two independent queue sources contribute to one graph. |
| Subscription service | Two keys receive queue updates, removal, and recreation. |
| Switched subscriptions | The output follows the selected queue-backed key through the source updates. |
| Mapped reference clients | Mapped clients receive queue updates under their original keys. |
| Request/reply service | Two clients receive queued replies for their requests. |
| Mapped request/reply clients | Queued replies retain the map's input keys. |
| Catalogue reuse | Each graph run starts a fresh queue-backed implementation. |

The switched case has five selector inputs and captures eight output ticks.
The runner drains literal input timers before queued updates, so the trace is
not one output position per selector input. The expected trace includes the
invalid output after the selected key is removed and its later value after
recreation.

The switched case establishes selected-output behavior, not service request
withdrawal. Existing
[`service_test.cpp`](../../../tests/evaluation/service_test.cpp) cases
"service clients in map branches withdraw requests when branches stop" and
"service clients follow repeated switch changes after a map branch returns"
assert request removal and re-addition directly through the service
implementation input.

## Classification

All nine added cases pass. No BG failure was reproduced. These cases exercise
queue sources returned by service implementation graphs and their use by
reference, subscription, request/reply, map, and switch clients. They do not
claim support for queue sources in arbitrary nested graphs.

## Verification

- Focused runner: **9 cases, 12 assertions passed**.
- Full HG test suite not run for this batch.
- The normal CMake test source list and development report index are unchanged;
  the root integration lane must register this test source and index this
  report.

[upstream]: https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8

# hgraph service composition and scalar feedback sweep, 2026-11-03

This batch extends the [services and feedback sweep](hgraph-service-feedback-sweep-2026-11-02.md)
with service chains, recursive requests, multiservice ordering, and feedback of
hierarchical scalars. The initial HG baseline is `2a44f374c`. The final review
is refreshed onto `63957337b`; the intervening changes affect web packaging,
installation checks, and documentation. Changes in this batch are limited to
tests and this report.

## Sources

The source snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and the development snapshot used for the 0.8 patterns at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
Source files are read from committed Git objects.

| Source | Requirement selected for review |
| --- | --- |
| 0.5 `hgraph_unit_tests/_wiring/test_service.py::test_two_services` | A mapped request/reply implementation calls another service. |
| 0.5 `test_recursive_request_reply_service`; 0.8 `python/tests/ported/_wiring/test_service.py::test_mapped_request_reply_service_can_call_itself_recursively` | Recursive mapped requests terminate at a base case and return through the callers. |
| 0.8 `tests/cpp/test_service_wiring.cpp`, service-interface permutation case; `python/tests/test_service_timing_semantics.py::test_multi_interface_service_flavours_are_order_independent` | Reference, subscription, and request/reply claims share one implementation independently of declaration and client order. |
| 0.8 `python/tests/test_service_timing_semantics.py::test_root_service_materialization_does_not_capture_a_child_context` | A lazily wired root service remains independent of a child context with the same name. |
| 0.8 `tests/cpp/test_feedback.cpp`, concrete Bundle alternatives cases | Feedback preserves a hierarchical scalar's concrete type and fields across updates. |

Expected deltas follow HG's [service](../design/services.md) and
[feedback](../design/evaluation.md#feedback) contracts. HG uses explicit
`feedback_service_implementation` to break request dependency cycles. Subscription
keys require the final guarantee. Upstream implicit delay counts do not define
HG reply timing.

## Contract boundaries

- [Service installation arguments](../design/services.md#implementation-multiplicity)
  can supply a root time series to a service implementation. The current HG
  contract does not specify capture of the lexical context active during
  catalogue registration. The upstream root-context case therefore needs a
  separate classification from ordinary request/reply failures.
- Literal runner sources publish ordinary time series. Subscription tests need
  a final or const key source rather than declaring that arbitrary literal
  ticks have the final guarantee. Reference-valued map outputs use a normal
  `copy_tick` node before capture, as required by the
  [capture contract](../testing.md).
- HG feedback has no initial-value argument. Upstream cases that initialize
  feedback are excluded; passive prior-state fallback is covered by the
  [preceding batch](hgraph-service-feedback-sweep-2026-11-02.md).
- The upstream feedback realization options select compact or pooled storage.
  HG has no equivalent public option. The selected tests check concrete scalar
  alternatives through HG's normal runtime representation.

## Added coverage

The service cases are in
[`service_composition_test.cpp`](../../../tests/library/service_composition_test.cpp).

| Area | Assertions |
| --- | --- |
| Service chain | The downstream implementation is registered first; sparse upstream requests produce delayed transformed replies. |
| Shared request/reply implementation | Two clients receive distinct replies; asymmetric updates affect only the corresponding output slot. |
| Multiservice ordering | All 18 upstream interface orders; every output slot and exactly one implementation wiring per order. |
| Recursive requests | A request decrements to zero and returns through the recursive callers; evaluation has an explicit time bound. |
| Installation input and child context | The root installation input and a child context with the same parameter name remain distinct as both update. |
| Subscription lifetime | Late clients, removal, and recreation with a different final key; invalid items before delayed replies. |

The feedback cases are in
[`feedback_alternatives_test.cpp`](../../../tests/evaluation/time_series/feedback_alternatives_test.cpp).
They start with a base scalar delayed by one cycle. Further cases check small
and large derived scalars, sparse changes between concrete alternatives,
sampling through a reference, and transport across a nested graph boundary.
Assertions compare concrete types and complete scalar values.

## Feedback findings

All six hierarchical-scalar feedback cases pass. They add no hidden
reproductions. The feedback source replays serialized ticks into the declared
base time series while retaining the concrete scalar type. A derived scalar's
additional fields remain present after replay, including when successive values
use different concrete types.

## Service contract differences

The unadapted service chain creates a dependency cycle. The downstream mapped
graph both submits a request to the upstream service and consumes its reply.
HG places the request provider after its clients. Reading those requests to
produce the reply adds the opposite dependency. Registering the upstream
implementation through `feedback_service_implementation` supplies the documented
one-cycle break. The adapted case asserts the complete sparse reply trace.

Classification: documented contract difference from upstream implicit service
feedback. The raw cycle rejection is not retained as a failing reproduction.

The root-context counterpart passes the root time series as an installation
argument. A mapped child establishes a same-named lexical context and reads it
through a normal context input. Its output observes both the child context and
the separately installed root value. Automatic capture of registration-time
lexical context remains an API/design gap relative to the upstream pattern;
this batch does not introduce that behavior.

## Finding: recursive service replies cross their own nested owner

The recursive request/reply case still fails during compilation after applying
`feedback_service_implementation`. Its terminating branch returns `1`; the
recursive branch calls the same service with `value - 1`, then adds `1` to the
reply. A request of `3` should therefore eventually produce `4`.

The compiler reports:

```text
reference[16]
  -> recursive_service_impl[10]/feedback_service_implementation[11]/nested[15]
  -> reference[16]
```

The wrapper delays generated requests, then runs the implementation in
[`nested()`](../../../include/hg/service_feedback.hpp). The implementation's
self-call imports its own service reply as an inherited time series.
[`nested()`](../../../include/hg/nested.hpp) adds inherited providers to the
outer node's inputs. The catalogue supplies that reply from the same nested
node's output. [`GraphWiring::provide_inherited_time_series`](../../../src/graph_wiring.cpp)
materializes the reply reference; resolving inherited inputs makes the outer
node depend on that reference. The resulting two-node cycle remains even
though requests are delayed.

Classification: wiring limitation in the feedback wrapper and nested reply
boundary. The compiler correctly rejects the topology it receives. Supporting
this upstream recursive pattern requires a design decision about the reply
boundary. The hidden `[.][repro]` case asserts the intended successful
trace. No graph evaluation starts, and no production code changed.

## Verification

The batch adds 12 test cases: six service-composition cases and six
hierarchical-scalar feedback cases. Eleven pass; the recursive service case
is a hidden reproduction. The multiservice case runs all 18 selected interface
orders.

The focused runner includes the new cases and their existing service,
multiservice, subscription, reference-service, and feedback neighbors:

- Ordinary run: **96 cases, 249 assertions passed**.
- Explicit recursive reproduction: **one case fails** with the dependency
  cycle above.
- Structured coverage tags validate with `tools/test_coverage.py --check`.
- `git diff --check`, the new C++ files' 120-column limit, and the report/index
  local link targets and anchors pass.

Both new sources are registered in the normal CMake test targets. The full HG
suite was not run for this tests-only batch.

## Scope limits

This batch reviews selected graph patterns rather than executing the complete
upstream suites. Generic multiservice specialization, service-owned push
sources, and the complete source-by-consumer product remain outside this
batch. Existing HG tests cover parts of those areas; this report does not
classify the unreviewed combinations as unsupported.

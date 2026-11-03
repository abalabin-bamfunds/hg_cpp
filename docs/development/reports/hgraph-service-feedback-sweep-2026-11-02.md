# hgraph services and feedback sweep, 2026-11-02

This batch extends the [reference-consumer sweep](hgraph-reference-consumer-sweep-2026-11-02.md)
with services and feedback across nested graph boundaries. The HG baseline is
`362c103dd` (current `main` at the start of this batch). Changes are limited to
tests and development reports. The [2026-11-03 follow-up](#higher-order-output-resolution-follow-up-2026-11-03)
records the subsequent wiring fix.

## Sources

The source snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and the hgraph development snapshot used for the 0.8 patterns at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
Source files are read from committed Git objects.

| Source | Requirement selected for review |
| --- | --- |
| 0.5 `hgraph_unit_tests/_runtime/test_feedback.py::test_feedback`; 0.8 `tests/cpp/test_feedback.cpp` passive-consumption case | Feedback used as passive prior state; bounded evaluation and independent state. |
| 0.5 `hgraph_unit_tests/_wiring/test_service.py::test_subscription_service`; 0.8 `python/tests/ported/_wiring/test_service.py::test_subscription_client_samples_existing_value_when_key_becomes_valid` | Multiple subscribers, late subscription, and existing-value sampling. |
| 0.8 `python/tests/ported/_wiring/test_service.py::test_subscription_client_follows_a_late_mapped_bundle_value`; `test_service_timing_semantics.py::test_subscription_structured_response_stays_unbound_until_its_field_is_valid` | Reference-backed structured replies whose fields become valid later. |
| 0.8 `python/tests/test_service_timing_semantics.py::test_service_dependent_request_reply_retains_full_feedback`, `test_service_dependent_subscription_defers_only_its_key_relay` | Request/reply and subscription implementations that depend on a reference service. |
| 0.8 `python/tests/test_service_timing_semantics.py::test_request_reply_inside_a_mapped_switch_keeps_late_keys`, `test_request_reply_switch_flip_removes_transiently_invalid_map_output` | Late map keys, switch replacement, and temporarily invalid replies. |

These sources select graph patterns. Expected deltas follow HG contracts.
Additional collection-feedback cases extend the upstream scalar feedback
pattern to HG's supported collection types.

## Contract differences and exclusions

- HG [feedback](../design/evaluation.md#feedback) has no initial-value argument.
  A passive optional prior-state input can supply an application default. This
  adapts the accumulator pattern; it does not port initialized feedback.
- HG [subscription keys](../design/services.md#service-declarations) require a
  final guarantee. Mutable-key upstream cases cannot be copied directly.
  Const keys and branch-local map keys can exercise late clients and withdrawal.
- HG request/reply paths use explicit
  [`feedback_service_implementation`](../design/services.md#implementation-multiplicity)
  when reading requests would otherwise create a dependency cycle. Upstream
  implicit delay counts do not establish an HG timing requirement.
- HG [map output](../design/map.md#output) retains keys with invalid targets by
  default. `"__valid_items__"_n = true` selects removal while a target is invalid.
  Upstream valid-only map traces require this option.

## Added cases

The batch adds 15 ordinary cases and two hidden reproductions in
[`service_patterns_test.cpp`](../../../tests/library/service_patterns_test.cpp)
and [`feedback_patterns_test.cpp`](../../../tests/evaluation/time_series/feedback_patterns_test.cpp).
The service cases belong to the library test binary because their graphs use
library arithmetic and delay operators. The feedback cases use the engine API.

| Area | Assertions |
| --- | --- |
| Basic service composition | Scalar reply transformations and a reference-service dependency. |
| Mapped switches | Late keys and removal while a delayed reply is invalid; exact reply timing. |
| Subscription replies | A reference-service dependency and a delayed field in a free bundle. |
| Shared subscriptions | Late clients, one-client withdrawal, last-client withdrawal, and re-addition. |
| Collection feedback | Independent bundle fields, set membership, and sparse fixed-list items. |
| Mapped feedback | Delayed values across removal and recreation; independent accumulator state. |
| Passive feedback | Optional prior state, sparse input, and quiescence after the final input. |
| Output inference | A declared-output passing service counterpart and hidden reproductions through a service wrapper and direct `nested()`. |

Graph construction, input deltas, and expected output remain visible in each
case. Assertions include invalid items and quiet cycles. New behavior checks
observe graph output rather than external value logs.

## Finding: inferred outputs across a nested boundary

At the reviewed baseline, a direct service implementation could infer its output
from a mapped reply graph. Wrapping that implementation in
`feedback_service_implementation` reached `nested()` and failed during wiring
with `type variable has no type map entry`. An explicit output declaration
passed with the expected delayed result `[SKIP, 2]`. A second reproduction
isolated `nested()` with a graph returning `operators::add(value, 1)`. Its
declared-output counterpart produced `[2, 3]`; the inferred-output call failed
before evaluation.

The [graph output-inference contract](../design/graph-wiring.md#graph-templates)
requires wiring to invoke a graph body once when its output is still unmapped,
then resolve that output from the returned wiring point.
At that baseline, [`nested_details::wire`](../../../include/hg/nested.hpp) called
`graph.signature().resolve_wiring_arguments(...)` before invoking the graph.
[`Signature::resolve_wiring_arguments`](../../../src/signature.cpp) completes
output resolution immediately. The body could not provide the missing mapping.
`SignatureWiringTemplate` already forwards `allows_output_inference()`; the
early-resolution path belonged to `nested()`.

Classification: wiring implementation bug under the existing graph contract.
Both reproductions assert the intended successful output. They initially failed
with the same missing-type-mapping error and used hidden `[.][repro]` cases.
The [follow-up](#higher-order-output-resolution-follow-up-2026-11-03) resolves
this finding. No runtime sampling defect was found in the tested compositions.

## Verification

The review lane uses native Release libraries rebuilt from the refreshed HG
baseline. Focused runners passed:

| Group | Passing cases | Passing assertions |
| --- | ---: | ---: |
| Service and feedback cases, including existing neighboring tests | 82 | 158 |
| Previous reference-consumer library group | 268 | 517 |
| Previous reference-consumer engine group | 455 | 1,210 |
| Total | 805 | 1,885 |

The service and feedback runner was independently built and executed after
consolidation into the review lane. Selecting `[repro]` executes both hidden
cases: both fail with the missing-type-mapping error, and the declared nested
counterpart passes. Neither reproduction crashes.

Coverage-tag validation passes for the new group and the previous library
group. The previous engine group retains the five baseline tag diagnostics
recorded in the [reference-consumer report](hgraph-reference-consumer-sweep-2026-11-02.md#verification).
Whitespace and local report links pass validation. The complete HG suite was
not run for this batch. No production code changed.

## Scope limits

This batch does not execute the complete upstream service or feedback suites.
The 0.5 two-request/reply-service chain, recursive service implementations,
0.8 multiservice interface-order product, and root-context shadowing pattern
remain unreviewed by this batch. Existing tests establish simpler multiservice
combinations, not the full interface-order product. Polymorphic scalar feedback
alternatives also remain unreviewed.

## Higher-order output resolution follow-up, 2026-11-03

`nested()` now prepares input boundaries without resolving the graph signature
itself. It invokes the graph through the shared wiring machinery, which first
tries signature output resolution, then infers remaining output mappings from
the body result when necessary. The nested output boundary uses that validated
result. Temporary variadic captures retain their existing ownership: scalar
entries belong to the inner graph.

Both hidden reproductions are ordinary regression tests after the fix.

| Path | Output-resolution audit |
| --- | --- |
| `nested()` | Removed the eager signature resolution; uses normal graph invocation. |
| `map()` | Invokes the graph through shared wiring after creating input boundaries in `MapGraphArgumentBuilder::build()`. |
| `mesh()` | Uses the map graph builder and forwards output-inference support through `ScopedWiringTemplate`. Dependency selection still requires the output type before selection wiring. |
| `switch_()` | Invokes each case through shared wiring before combining its validated output type with the other cases. |
| `reduce()` | Invokes the reducer through shared wiring. Its documented compact-storage probe can wire a separate graph a second time. |
| `dispatch_()` | Retains `infer_output` in the selected implementation resolution and uses shared result validation. Its declared output contract must resolve before branch wiring, as documented. |
| Service wrappers | Their eager completion resolves the declared service contract. Implementation graphs use normal invocation; the feedback wrapper reaches the corrected `nested()` path. |

No other instance of premature implementation-output resolution was found in
these paths. Added cases exercise inferred outputs through `map`, `switch_`,
and `reduce`. The nested regressions check both successful inference with one
body invocation and preservation of an output resolver failure before body
invocation.

The review lane rebuilt the affected Release test runners against matching
native libraries. All focused cases passed:

| Group | Passing cases | Passing assertions |
| --- | ---: | ---: |
| Graph and engine behavior, including nested graphs and contexts | 458 | 1,213 |
| Services and feedback | 85 | 165 |
| Graph templates, variadic wiring, map/switch/reduce wiring, and dispatch | 174 | 538 |
| Previous reference-consumer library group | 268 | 517 |
| Total | 985 | 2,433 |

The implementation lane also passed its combined service/map/switch/reduce
runner: 205 cases and 336 assertions. Those cases overlap the review-lane groups
and are not added to the total. The variadic group includes the existing check
that captured scalars are owned only by the inner graph; the graph-template
group includes seeded nested output types.

All six added or promoted regression cases have valid coverage tags. The
service-and-feedback group passes tag validation. The engine group retains its
five baseline diagnostics. The additional wiring group has one existing
diagnostic: `map retains named and unnamed scalar arguments from the caller
tuple` uses the undocumented `lifetime` dimension. Whitespace and changed
documentation links pass validation. The complete HG suite was not run.

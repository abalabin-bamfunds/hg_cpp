# hgraph services and feedback sweep, 2026-11-02

This batch extends the [reference-consumer sweep](hgraph-reference-consumer-sweep-2026-11-02.md)
with services and feedback across nested graph boundaries. The HG baseline is
`362c103dd` (current `main` at the start of this batch). Changes are limited to
tests and development reports.

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

A direct service implementation may infer its output from a mapped reply graph.
Wrapping that implementation in `feedback_service_implementation` reaches
`nested()` and fails during wiring with `type variable has no type map entry`.
An explicit output declaration passes with the expected delayed result
`[SKIP, 2]`. A second reproduction isolates `nested()` with a graph returning
`operators::add(value, 1)`. Its declared-output counterpart produces `[2, 3]`;
the inferred-output call fails before evaluation.

The [graph output-inference contract](../design/graph-wiring.md#graph-templates)
requires wiring to invoke a graph body once when its output is still unmapped,
then resolve that output from the returned wiring point.
[`nested_details::wire`](../../../include/hg/nested.hpp) calls
`graph.signature().resolve_wiring_arguments(...)` before invoking the graph.
[`Signature::resolve_wiring_arguments`](../../../src/signature.cpp) completes
output resolution immediately. The body cannot provide the missing mapping.
`SignatureWiringTemplate` already forwards `allows_output_inference()`; the
remaining early-resolution path belongs to `nested()`.

Classification: wiring implementation bug under the existing graph contract.
Both reproductions assert the intended successful output and fail with the
same missing-type-mapping error. They use hidden `[.][repro]` cases. No runtime
sampling defect was found in the tested compositions.

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

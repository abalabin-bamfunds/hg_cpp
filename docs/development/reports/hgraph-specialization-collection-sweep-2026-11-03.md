# hgraph generic services and collection composition sweep, 2026-11-03

This batch extends the [service composition sweep](hgraph-service-composition-sweep-2026-11-03.md)
with generic service specializations and collection conversion, accumulation,
emission, and library reductions. The HG baseline is `31588bdbd`. The review
lane rebased onto that revision and rebuilt its native Release libraries before
running the tests. Changes are limited to tests and development reports.

## Sources

The source snapshots are hgraph 0.5.42 at
[`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`](https://github.com/hhenson/hgraph/tree/c34dba1db8a1c873ee303796db4c8fc48c8cc5be)
and the development snapshot used for the 0.8 patterns at
[`d36be500ec8b6fda77ff077566f891ffcf5c84a8`](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8).
Sources are read from committed Git objects. The new cases combine selected
upstream patterns through HG's supported public APIs; they are not literal
ports of the upstream test suites.

| Source | Selected pattern |
| --- | --- |
| 0.8 `python/tests/ported/_wiring/test_service.py::test_generic_multi_service_reuses_shared_registration_specialization`, `test_same_named_service_specializations_can_have_different_reply_schemas` | One service identity supports distinct resolved types and output schemas; multiple clients share implementations. |
| 0.8 `python/tests/test_generic_service_specializations.py::test_generic_multi_service_materializes_each_requested_specialization`; `test_service_client_options.py::test_request_reply_service_client_supplies_scalar_options` | Generic implementation multiplicity and scalar service arguments. |
| 0.8 `python/tests/test_service_timing_semantics.py::test_request_reply_inside_a_mapped_switch_keeps_late_keys`, `test_request_reply_switch_flip_removes_transiently_invalid_map_output`, `test_subscription_inside_a_mapped_switch_keeps_late_keys` | Request/reply and subscription clients cross mapped switch boundaries, including invalid output intervals. |
| 0.5 `_conversion_operators/test_set_conversion_operators.py::test_convert_tss_to_set`, `test_collect_set` | Convert complete membership to scalar snapshots and accumulate scalar events. |
| 0.5 `_conversion_operators/test_mapping_conversion_operators.py::test_convert_tsd_to_mapping`, `test_collect_mapping`, `test_emit_mapping` | Convert dictionary values, accumulate keyed events, and emit key/value entries. |
| 0.5 `_conversion_operators/test_tuple_conversion_operators.py::test_convert_tsl_to_tuple`, `test_collect_scalar_to_tuple`, `test_emit_tuple` | Convert fixed lists and accumulate and emit scalar sequences. HG uses scalar `List` for homogeneous sequences. |
| 0.8 `tests/cpp/test_collection_nodes.cpp`, TSL/TSD unary reduction cases; `tests/cpp/test_reduce.cpp`, switched keyed result and downstream-map case | Reduce current collection values after updates and removals; consume switched keyed outputs. |

The abbreviated 0.5 paths are under `hgraph_unit_tests/_operators/`.

## Added service coverage

[`service_specialization_patterns_test.cpp`](../../../tests/library/service_specialization_patterns_test.cpp)
checks generic service signatures through concrete top-level graph calls.

| Composition | Assertions |
| --- | --- |
| Generic reference multiservice | One service identity supplies two distinct named bundle schemas; both fields are captured and checked. |
| Generic request/reply multiservice | Scalar claim filters and installation values select replies for distinct resolved scalar types; an unregistered scalar/type combination is rejected. |
| Shared generic reference claims | Repeated calls share one implementation; the same catalogue builds independent graphs without retaining the preceding build's wiring. |
| Generic request/reply inside map and switch | Delayed replies, ordinary branch output, client withdrawal, and recreation are checked in one complete trace. |
| Generic subscription key types | Integer and string specializations serve their own mapped keys through explicit request feedback. |
| Scalar-filtered subscription inside map and switch | Replacing the selected route withdraws the old clients and produces replies for the new route after the request delay. |

Heterogeneous service results are captured in named bundle fields. Tests compare
each field directly. The implementation wiring counter observes the catalogue's
build-scoped wiring operation; runtime behavior is checked through graph output.

## Added collection coverage

[`collection_composition_patterns_test.cpp`](../../../tests/library/collection_composition_patterns_test.cpp)
starts with scalar snapshot conversion and then combines more operators.

| Composition | Assertions |
| --- | --- |
| TSS to scalar Set to sum | Additions and removals update the complete snapshot; an empty set sums to zero. |
| Fixed TSL to scalar List to sum | A sparse item update preserves the other item's value. |
| Collect Set to TSS to sum | Duplicate scalar ticks do not create membership changes; a false reset tick clears prior membership before inserting the simultaneous value. |
| Collect Dict to TSD to sum | Key replacement and reset preserve only the current accumulated values. |
| TSD to scalar Dict to emit | Named key/value output fields, a quiet empty-dictionary cycle, and re-addition. |
| Map to emit to collect Set to sum | Transformed value events accumulate across source-key removal and recreation. |
| Nested collect List to outer emit | Reset changes future snapshots while previously queued entries drain in FIFO order. |
| Switch to map to sum | Switching samples the selected dictionary's current values; sparse changes and removals reach the total. |

These cases exercise library reductions through `operators::sum_`. They do not
add coverage of higher-order `reduce()` reducer graphs.

## Contract boundaries

Request-dependent implementations called from nested clients use
[`feedback_service_implementation`](../design/services.md#implementation-multiplicity).
The wrapper delays requests before evaluating the implementation. The tests
retain each selected service branch long enough to receive a reply, and check
invalid output items before those replies arrive. Subscription keys come from
map's selected keys with their final guarantee; runner literals do not assert
that guarantee.

The request/reply map selects `__valid_items__ = true`. Pending replies are
omitted from its output. Returning to the service branch removes the preceding
valid entry until the new reply arrives.

[Collection conversion and emission](../design/library.md#collection-accumulation-and-emission)
distinguish complete scalar snapshots from time-series collection changes.
An empty scalar dictionary contributes no emit entries. With no queued work,
that cycle is `SKIP`; it does not invalidate the preceding output. Set and
dictionary iteration order is unspecified, so dictionary emission cases use
one entry per snapshot and assert both named fields.

A collector reset clears its history before inserting a simultaneous value.
It does not discard entries already queued by a downstream emitter. The nested
case asserts the complete six-cycle emission trace, including work that drains
after the final input.

The preceding batch's recursive service case is a
[parity corner case](hgraph-service-composition-sweep-2026-11-03.md#recursive-service-adaptation-requires-reply-feedback).
It requires reply feedback in addition to request feedback and is outside this
batch. It is not classified as an HG bug.

## Findings and verification

All 14 new cases pass: six service cases with ten assertions, and eight
collection cases with eight assertions. No new HG defect was established.
Initial service draft failures were resolved by using the existing feedback
adapter and matching implementation signatures to forwarded service scalars.
Those corrections adapt the tests to the current contract; no production
changes were made.

The review lane independently built and ran both combined focused groups:

| Group | Passing cases | Passing assertions |
| --- | ---: | ---: |
| Services and feedback, including the new specialization cases | 102 | 259 |
| Conversion, collect, emit, scalar collections, and collection reductions | 95 | 281 |
| Total | 197 | 540 |

The previous batch's hidden recursive-service parity case is excluded from the
ordinary service run. The new cases add no hidden reproductions.

All 14 new cases have valid structured coverage tags. The combined service
group passes tag validation. The collection group retains two baseline tag
diagnostics from `scalar_collection_test.cpp`: `scalar:double` and
`transition:retargeting` are not documented dimension values. The same two
diagnostics occurred before adding these cases.

Both new sources are registered in `hg_library_tests`. Whitespace, the new C++
files' 120-column limit, and report/index link targets and anchors pass checks.
The full HG suite was not run for this tests-only batch.

## Scope limits

This batch checks selected supported compositions. It does not execute the
complete upstream suites or establish the full source-by-consumer product.
The catalogue's independent-build check extends the upstream generic service
pattern rather than reproducing a named upstream test.

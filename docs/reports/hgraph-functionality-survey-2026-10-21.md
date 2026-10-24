# BG and hgraph functionality survey

This survey compares the committed BG tree with the current native hgraph tree
and the available legacy Python hgraph release. It focuses on working behavior
and public workflows. A declared type or extension hook alone does not count as
complete support.

## Findings

BG already implements a broad graph engine. Its strongest overlap with hgraph
is the runtime core: scalar and generic type resolution, value and collection
time series, nested graph evaluation, dynamic map/mesh/switch/reduce, services,
feedback, concurrent queue input, and profiling. The C++ surface is the primary
BG implementation; the Python package authors graphs and nodes against the
same engine.

The clearest differences are around ecosystem breadth and user tools. hgraph
0.8 includes rolling windows, Arrow-backed frame and array values, a much wider
standard operator catalogue, distributed map, graph inspection, and checkpoint
and persistence facilities. BG has a useful profiler and retained-delta
serialization, but the latter is runtime plumbing rather than an application
record/replay or checkpoint workflow. BG also has no comparable first-party
adapter suite in this tree. These are separate workflow gaps; they do not imply
that BG's common graph-evaluation model is missing.

BG's Python surface is usable for graph and node authoring, type annotations,
collection deltas, and C++ node interoperation. It remains narrower than the
legacy Python hgraph 0.5.42 package and the hgraph 0.8 ecosystem. The legacy
comparison is about Python workflows only: hgraph 0.5.42 was Python-first and
included optional C++ integration, whereas hgraph 0.8 uses a C++ runtime with
Python authoring.

No performance comparison was performed. The test references below identify
behavioral coverage; this survey did not run tests or builds.

## Scope and status terms

The BG baseline is commit `b0c78ea6f49a449e25c3b6be31d922ee048b894d`. The
primary hgraph baseline is committed HEAD
`d36be500ec8b6fda77ff077566f891ffcf5c84a8`, dated 2026-10-17. Its local
worktree has unrelated uncommitted changes; only committed HEAD objects were
read. The legacy Python reference is hgraph `v_0.5.42`, commit
`c34dba1db8a1c873ee303796db4c8fc48c8cc5be`. The hgraph repository's origin is
`https://github.com/hhenson/hgraph`; source links below pin those revisions.

`Supported` means a public path, implementation, and behavior tests were
located. `Partial` means the capability exists with a material limitation in
the stated scope. `Absent` means no public implementation was found in the
pinned tree. `Unverified` means available evidence does not establish usable
behavior. BG's status columns distinguish native C++ from Python exposure.

| hgraph capability | BG C++ | BG Python | Practical limitation and evidence |
| --- | --- | --- | --- |
| Scalar types, schemas, and generic resolution | Supported | Supported | BG provides primitive, list, set, dictionary, struct, hierarchical, fixed-tuple, and extension scalar paths with type-variable resolution. This row covers the common scalar model; Arrow-backed analytics values are listed separately. [BG type model](../type-system.md#scalar-types), [scalar tests](../../tests/value/scalar/tuple_test.cpp), [Python type tests](../../bindings/python/tests/test_tuple_scalars.py); [hgraph types](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/types.rst), [hgraph tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_value_types.cpp). |
| TS, fixed TSL, TSD, TSS, TSB, REF, validity, guarantees, and deltas | Supported | Supported | BG covers nested collections, additions/removals, references, and per-input validity/guarantees. Fixed-size TSL is supported, matching hgraph's documented `TSL[..., Size[N]]` form; BG's unsized `TSL<TS_T>` declaration has no runtime storage or node view. hgraph has a distinct rolling `TSW`; BG has no corresponding type. A dedicated `SIGNAL` name is an API difference whose semantic equivalence to a generic BG input was not assessed. Both engines' `all_valid` checks are shallow over immediate children. [BG type docs](../time-series-types.md), [validity docs](../time-series-validity.md#nodes), [BG validity tests](../../tests/evaluation/validity/all_valid_input_test.cpp); [hgraph type docs](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/types.rst), [hgraph time-series tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/python/tests/test_time_series_api_contract.py). |
| Node and graph authoring, overloads, and type constraints | Supported | Supported | BG C++ has callable, aggregate, runtime-signature, graph-template, and operator-overload paths. Python exposes `@graph`, `@node`, `@compute_node`, `@source_node`, generic signatures, and operator wiring. Exact parity across the wider authoring APIs is not assessed. [BG node API](../node-api.md), [operator resolution](../operators.md), [Python API](../python.md#graph-authoring), [wiring tests](../../tests/wiring/operator_test.cpp), [Python authoring tests](../../bindings/python/tests/test_node.py); [hgraph Python authoring](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/python/index.rst). |
| Standard operators | Partial | Partial | BG has tested numeric, temporal, boolean, validity, sampling, timed, stream, and collection-reduction families. hgraph's catalogue additionally covers broader date/time, tuple, JSON, frame, array, and table operations. Python `bg.operators` exposes BG's native registry; it does not add the hgraph operator catalogue. [BG standard library](../library.md), [BG operator tests](../../tests/library/operators_test.cpp), [hgraph operator catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst). |
| Arrow-backed analytics values and operations | Absent | Absent | hgraph has native `Frame` and `Array` scalar schemas and operators for JSON, data frames, and tables. BG's surveyed tree has no corresponding Arrow-backed value family. [hgraph types](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/types.rst), [hgraph data and analytics](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/data_and_analytics.rst), [hgraph operators](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst). |
| Dynamic graph composition: map, mesh, switch, and reduce | Supported | Supported | BG provides all four in C++ and through the Python namespace. Map, mesh, and switch own dynamic child graph lifecycles; reduce supports compact and nested evaluation paths. hgraph also provides distributed map, which BG lacks. [BG map](../map.md), [mesh](../mesh.md), [switch](../switch.md), [reduce](../reduce.md), [BG tests](../../tests/evaluation/reduce_test.cpp); [hgraph tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_distributed_map.cpp). |
| Evaluation, scheduling, lifecycle, and failure handling | Supported | Supported | Both expose simulation and real-time execution, timers, lifecycle, and error paths. BG additionally documents externally driven stepping and strict queue/timer ordering; hgraph has broader checkpoint/recovery integration. [BG evaluation](../evaluation.md), [BG scheduler tests](../../tests/evaluation/engine), [hgraph runtime tests](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp). |
| Services, shared state, and feedback | Supported | Partial | BG C++ has request/reply, subscription/reference, multiservice wiring, run-scoped shared `GlobalState`, and feedback. Python exposes service definitions and per-node `State[T]`; no public Python binding for graph-scoped `GlobalState` or feedback wiring was found. [BG services](../services.md), [global state](../global-state.md), [feedback tests](../../tests/evaluation/time_series/feedback_test.cpp), [Python service tests](../../bindings/python/tests/test_services.py), [Python node parameters](../python.md#python-nodes); [hgraph service tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_service_runtime.cpp). |
| Concurrent queue input | Supported | Supported | BG supports concurrent writes through queue handles and applies framed tick messages at graph cycle boundaries; its Python binding has queue tests. hgraph has push-source queues too. These are source-ingress APIs, not a general async graph execution model. [BG queue docs](../evaluation.md#queue-input), [queue tests](../../tests/evaluation/time_series/runtime_graph_queue_test.cpp), [Python tests](../../bindings/python/tests/test_queue.py); [hgraph queue tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_push_source_queues.cpp). |
| Python conversion and C++ node interoperability | Supported | Supported | Common scalar and collection conversions, Python callbacks, and native C++ node calls are supported. An unregistered Python class becomes a Python-object scalar on first use; extensions can register conversions for engine-native scalar types. [BG Python bridge](../python.md#runtime-boundary), [native node tests](../../bindings/python/tests/test_native_cpp_nodes.py), [Python scalar tests](../../bindings/python/tests/test_python_object_scalars.py); [hgraph bridge tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/python/tests/test_python_node_native_io.py), [legacy C++ integration tests](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph_unit_tests/_types/test_cpp_type_integration.py). |
| Graph testing | Supported | Supported | BG `run_graph` supplies typed scalar and collection deltas and compares captured nested output deltas; Python has the analogous graph runner. hgraph offers `eval_node` and graph testing in both eras. Test API differences are ergonomic, not missing runtime support. [BG testing](../testing.md), [test graph tests](../../tests/testing/graph_run_test.cpp), [Python graph tests](../../bindings/python/tests/test_graph_collection_inputs.py); [hgraph C++ testing guide](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/cpp/testing_graphs.rst), [legacy test runner](https://github.com/hhenson/hgraph/blob/c34dba1db8a1c873ee303796db4c8fc48c8cc5be/hgraph/test/__init__.py). |
| Profiling, tracing, inspection, and diagnostics | Partial | Partial | BG has a usable evaluation profiler and wiring/evaluation diagnostics, with Python runner profiling. The surveyed BG tree has no equivalent live graph inspector or full lifecycle trace UI. hgraph 0.8 documents and tests its inspector and profiling/tracing tools. [BG profiling](../evaluation.md#evaluation-profiling), [profiler tests](../../tests/evaluation/engine/evaluation_profiler_test.cpp), [Python profiler tests](../../bindings/python/tests/test_runner_profiler.py); [hgraph inspector](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/tools/inspector.rst), [inspector tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/python/tests/test_inspector.py). |
| Tick retention, user-facing record/replay, and checkpoint recovery | Partial | Absent | BG serializes and replays supported time-series deltas for runtime operators and queues; it has no public durable graph record/replay or checkpoint/recovery workflow. hgraph 0.8 has in-memory record/replay and checkpoint support, with durable component recovery in its persistence extension. Do not equate BG's serializer with those products. [BG tick format](../time-series-types.md#retaining-ticks), [serializer tests](../../tests/evaluation/time_series/tick_serializer_test.cpp); [hgraph record/replay tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_record_replay.cpp), [checkpoint tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_component_checkpoint.cpp), [persistence extension](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8/extensions/persistence). |
| Distributed execution | Absent | Absent | hgraph 0.8 exposes externally driven child graphs and distributed map across worker processes. BG queue concurrency is within one process and does not provide distributed graph execution. [hgraph distributed-map guide](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/distributed_map.rst), [implementation tests](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/tests/cpp/test_distributed_map.cpp). |
| External adapters, persistence, and dataframe integration | Absent | Absent | BG exposes provider and extension contracts, but the pinned tree contains no comparable first-party SQL, Kafka, web, dataframe, or persistence adapters. hgraph 0.8 ships adapter packages and separate extensions. This is ecosystem breadth, not a core graph semantic. [BG provider API](../time-series-providers.md), [hgraph adapters](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/adaptors/index.rst), [extensions](https://github.com/hhenson/hgraph/tree/d36be500ec8b6fda77ff077566f891ffcf5c84a8/extensions). |
| Packaging and supported platform contract | Partial | Partial | BG has a buildable CMake engine and a scikit-build Python wheel path, but this survey found no hgraph-like committed cross-platform wheel support matrix or release gate. hgraph documents stable-ABI wheels and explicit platform validation. [BG Python build](../python.md#build-and-install), [BG package metadata](../../pyproject.toml); [hgraph release contract](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/developer_guide/release_readiness.rst). |

## Operator families

These examples show the breadth of representative families. They do not
measure implementation parity or performance.

| Family | BG examples | hgraph 0.8 examples | Difference visible in this survey |
| --- | --- | --- | --- |
| Arithmetic and comparison | `+`, `-`, `*`, `/`, `floordiv`, `pow`, `round`, `eq`, `lt` | `add_`, `sub_`, `mul_`, `div_`, `floordiv_`, `pow_`, `cmp_`, `eq_`, `datepart`, `temporal_round` | BG covers common numeric arithmetic and selected temporal operations; hgraph has a broader date/time surface and numeric promotion. [BG](../library.md#scalar-operators); [hgraph catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst) |
| Validity and selection | `valid`, `all_valid`, `sample`, `dedup`, `default_`, `filter_`, `if_then_else` | `valid`, `all_valid`, `sample`, `dedup`, `default`, `filter_`, `if_then_else`, `gate` | Strong common core; hgraph includes additional selection and gating operators. [BG](../library.md#validity-and-tick-selection); [hgraph catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst) |
| Stateful and timed | `count`, `lag`, `sums`, `delay`, `schedule`, `throttle` | `lag`, `delay`, `throttle`, `window`, `temporal_bucket`, `temporal_round` | BG supplies stateful calculations and scheduling primitives; hgraph includes a rolling-window family and more temporal transforms. [BG](../library.md#counting-and-one-event-lag); [hgraph catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst) |
| Collections | `sum_`, `min_`, `max_`, `mean`, `len_`, `contains_` | `sum_`, `min_`, `max_`, `mean`, `len_`, `contains_`, `group_by`, `partition`, `union`, `intersection` | BG has core reductions and queries; hgraph exposes a larger set and transform surface. [BG](../library.md#collection-reductions); [hgraph catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst) |
| Dynamic graph composition | `map`, `mesh`, `switch_`, `reduce` | `map_`, `mesh_`, `switch_`, `reduce`, `dispatch_` | Both have dynamic composition; hgraph adds dispatch and distributed map. [BG](../map.md); [hgraph distributed map](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/user_guide/distributed_map.rst) |
| Data interchange | No built-in JSON, Arrow frame, or table operator family found | `json_encode`, `json_decode`, `from_data_frame`, `to_data_frame`, `from_table`, `to_table` | Concrete data-conversion surface in hgraph; BG has provider and extension contracts but no matching built-in family. [hgraph catalogue](https://github.com/hhenson/hgraph/blob/d36be500ec8b6fda77ff077566f891ffcf5c84a8/docs/source/reference/operator_catalogue.rst), [BG providers](../time-series-providers.md) |

## Workflow gaps to consider

These are discussion priorities, not approved implementation work.

1. **Close specific type/operator workflows.** Decide whether BG needs rolling
   windows (`TSW`) and which additional operator families have user demand.
   A dedicated `SIGNAL` name is an API difference; whether generic BG inputs
   provide equivalent semantics remains unverified. The existing common
   time-series types and fixed-size TSL do not need a redesign.
2. **Measure representative end-to-end workloads.** Compare equivalent graph
   shapes and update patterns before prioritizing optimizations. The two
   projects use different storage, scheduling, and Python boundaries, so
   operator counts or source inspection cannot establish performance parity.
3. **Add an inspection workflow.** BG profiling answers where evaluation time
   is spent. A live graph inspector and selective per-node lifecycle trace
   would answer what is wired and what values/changes the graph observes.
4. **Choose an application persistence contract.** If users need recovery,
   define graph/component identity, supported state, storage boundary, and
   restart semantics. Keep low-level tick serialization as an implementation
   primitive, not as the user contract.
5. **Prioritize adapter demand.** Add a first-party adapter only for a concrete
   cross-domain need; otherwise keep integrations in separately installed
   extensions. Candidate workflows already present in hgraph include Kafka,
   SQL, HTTP/WebSocket, dataframe, and persistence.
6. **Make Python/release scope explicit.** Decide which legacy Python authoring
   and adapter workflows BG intends to support, then publish a tested Python
   and platform matrix. BG's current bridge is a working authoring surface,
   while hgraph's stable-ABI package policy is a more complete distribution
   contract.

## Evidence limits

This is a source and test survey, not a test run, API conformance suite, or
performance study. The hgraph comparison uses its committed 0.8 source rather
than the dirty working tree. The legacy 0.5.42 source is used only to establish
the earlier Python-first feature surface; no claim is made that every 0.5
adapter or behavior remains part of the current 0.8 API. Absence findings mean
the capability was not found in the pinned BG source, public headers, docs, or
tests searched for this survey.

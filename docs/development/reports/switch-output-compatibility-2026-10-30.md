# Switch output compatibility verification

The selected compatibility behavior is implemented in
`agent/switch-output-compatibility`, based on root revision `467c66b3`.
Full verification is not green: the original nested map/switch/`take`
reproduction now reaches a wiring crash after passing output inference.

## Implemented behavior

Switch output inference removes references recursively from branch targets
before computing their common type. Named and anonymous bundles can share a
common type when their flattened field names, count, and order match. The common
type preserves the named declaration, inheritance, and metadata, and combines
field guarantees.

The production diff changes only `src/switch.cpp` and
`src/time_series/tsb/tsb_type.cpp`. Runtime code, node counts, and runtime storage
are unchanged. See the
[decision record](../decisions/0094-switch-output-reference-compatibility.md).

## Verification

The Release build completed all configured targets, including Python, web, and
benchmarks. After restoring the original increment graph in the reproduction,
`bg_library_tests` rebuilt successfully. The final full CTest run executed all
2,948 entries: 2,946 passed, two failed, and none were skipped.

Seven new compatibility cases pass. They cover reference/value dictionary and
fixed-list branch inference, downstream value and reference consumers, native/free
fixed-list evaluation, native/free bundle evaluation, rebuilt free bundle
reselection, and named/anonymous bundle common types. Negative checks cover
incompatible keys, list sizes, bundle field order, missing fields, and field types.

Before rebasing, the same seven cases were checked against an ABI-compatible
older core: six failed and the fixed-list storage case passed. All seven passed
with the implementation. The baseline library and binary hashes are recorded
in `build/baseline/manifest.json`; that library must not be used with the rebased
binaries because the engine ABI has changed.

Final logs are in `build/release/full-ctest-final-console.log`,
`build/release/full-ctest-final.log`, and `build/release/full-ctest.xml`.

## Remaining nested wiring failure

[`take keeps the initial map of branches across nested switch replacement`](../../../tests/library/tick_selection_test.cpp)
preserves the original graph and expected output. It previously failed with
`switch case output types are incompatible`. It now passes that check and
receives SIGSEGV during wiring, before evaluation or output assertions.

GDB confirmed this call chain with the simpler increment node form:

```text
MapGraphArgumentBuilder::build()
  graph_wiring_structure()
    snapshot()
      SwitchNodeDefinition::definition_identity()
        graph_wiring_structure()
          snapshot()
            wired.node_template().definition_identity()
```

The last call is at `src/graph_wiring_structure.cpp:381`; the enclosing switch
identity snapshot is at `src/switch_node.cpp:182`. GDB observed a null indirect
call and a base `WiringTemplate` vtable at that call site. The exact cause is not
established. Both the original increment graph and its simpler
`node([](double value) { return value + 1.0; })` form reproduce the crash.

Run the retained, unhidden case with:

```sh
build/release/tests/bg_library_tests \
  'take keeps the initial map of branches across nested switch replacement' \
  --colour-mode none
```

The diagnostic artifacts are `build/release/nested-take-backtrace.log` and
`build/release/nested-take-callsite.log`. No wiring identity or lifetime fix is
included.

## Other full-suite failure

`bg_web_python_tests` has seven failing pytest cases and two passing cases.
Nanobind rejects BG native graph/service objects at the web extension boundary
with `incompatible function arguments`. This matches the previously observed
binding mismatch. No Python or web code is changed here.

# Switch output compatibility verification

The selected compatibility behavior is implemented in
`agent/switch-output-compatibility`. Initial full-suite verification used root
revision `467c66b3`; the subsequent crash investigation uses root `1b4a241c`.
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
call and a base `WiringTemplate` vtable at that call site. Both the original
increment graph and its simpler
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

## Wiring ownership investigation

After rebasing onto `1b4a241c` and rebuilding `bg_library_tests`, the original
regression still receives SIGSEGV. A scalar-only graph also reproduces the crash:

```cpp
const auto increment = node([](i64 value) { return value + 1; });

const auto tested = [=](wp<TS<bool>> key, wp<TS<i64>> value) {
    return switch_(key,
                   cases{case_(false, [increment](auto input) { return increment(input); }),
                         case_(true, [](auto input) { return input; })},
                   value);
};

(void)run_graph(tested, {false, true}, {1, 2});
```

The scalar reproduction does not use map, bundles, collection references, or
`take`. Its branch target types already agree without reference removal.
GDB records destruction of the captured increment node, followed by a
dereference of that exact address in `make_compilation_plan()`. The node's
virtual-table pointer is zero at the crash. This confirms a use-after-free.

The standalone source, binary, and evidence are retained in
`build/release/switch_lifetime_probe.cpp`, `build/release/switch_lifetime_probe`,
`build/release/switch_lifetime_probe-gdb.log`, and
`build/release/switch_lifetime_probe-dtors.log`.

The generic case adapter has an ownership gap:

1. `bound_switch_graph()` copies the case lambda, including its captured node,
   into a `GraphTemplate`.
2. `invoke_switch()` retains that wrapper only in its local `owned_graphs` vector.
3. Wiring the branch stores a raw pointer to the captured node in `WiredNode`.
4. Returning from `invoke_switch()` destroys the wrapper and captured node.
   The branch `GraphWiring` survives and retains the pointer.
5. A later structure snapshot or compilation dereferences the destroyed node.

The relevant code is
[`switch_invoke.hpp`](../../../include/bg/switch/details/switch_invoke.hpp),
[`WiredNode`](../../../include/bg/graph/wired_node.hpp), and
[`SwitchBranchWiring`](../../../include/bg/switch/details/switch_node.hpp).
The latter retains the wired branch without retaining the callable that owns
the captured node templates. In the original nested test, the false inner case
captures `empty_values` by value and has this ownership pattern.

The corrective ownership requirement is to retain the case callable and its
captured node templates for the lifetime of the branch wiring. This affects
wiring ownership; runtime evaluation and reference storage need no change.
No correction is implemented in this investigation.

## Other full-suite failure

`bg_web_python_tests` has seven failing pytest cases and two passing cases.
Nanobind rejects BG native graph/service objects at the web extension boundary
with `incompatible function arguments`. This matches the previously observed
binding mismatch. No Python or web code is changed here.

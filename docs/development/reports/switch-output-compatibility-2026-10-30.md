# Switch output compatibility verification

The selected compatibility behavior and switch case ownership correction are
implemented in `agent/switch-output-compatibility`, based on root revision
`7857816e`. The ownership correction removes the confirmed use-after-free.

## Implemented behavior

Switch output inference removes references recursively from branch targets
before computing their common type. Named and anonymous bundles can share a
common type when their flattened field names, count, and order match. The common
type preserves the named declaration, inheritance, and metadata, and combines
field guarantees.

The compatibility change affects `src/switch.cpp` and
`src/time_series/tsb/tsb_type.cpp`. Runtime code, node counts, and runtime storage
are unchanged. See the
[decision record](../decisions/0094-switch-output-reference-compatibility.md).

## Verification before the ownership correction

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

## Confirmed nested wiring failure

[`take keeps the initial map of branches across nested switch replacement`](../../../tests/library/tick_selection_test.cpp)
preserves the original graph and expected output. It initially failed with
`switch case output types are incompatible`. The compatibility correction let
it pass that check, then it received SIGSEGV during wiring, before evaluation
or output assertions.

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
`build/release/nested-take-callsite.log`.

## Wiring ownership investigation

After rebasing onto `1b4a241c` and rebuilding `bg_library_tests`, the original
regression still received SIGSEGV. A scalar-only graph also reproduced the crash:

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

The generic case adapter had an ownership gap:

1. `bound_switch_graph()` copied the case lambda, including its captured node,
   into a `GraphTemplate`.
2. `invoke_switch()` retained that wrapper only in its local `owned_graphs` vector.
3. Wiring the branch stored a raw pointer to the captured node in `WiredNode`.
4. Returning from `invoke_switch()` destroyed the wrapper and captured node.
   The branch `GraphWiring` survived and retained the pointer.
5. A later structure snapshot or compilation dereferenced the destroyed node.

The relevant code is
[`switch_invoke.hpp`](../../../include/bg/switch/details/switch_invoke.hpp),
[`WiredNode`](../../../include/bg/graph/wired_node.hpp), and
[`SwitchBranchWiring`](../../../include/bg/switch/details/switch_node.hpp).
Before the correction, the latter retained the wired branch without retaining
the callable that owns the captured node templates. In the original nested
test, the false inner case captures `empty_values` by value and had this
ownership pattern.

## Ownership correction

`case_()` creates shared ownership of its graph template or graph function.
The generic case adapter retains that function without copying it again.
`PreparedSwitchCase` passes ownership to `SwitchBranchWiring`, which destroys
its wired graph before releasing the callable. This keeps captured node
templates alive through structure comparison and compilation.

The type-erased `SwitchCaseArgument` interface retains its borrowed lifetime
contract. Runtime evaluation and reference storage are unchanged. See the
[decision record](../decisions/0095-switch-case-template-lifetime.md).

Two scalar regressions exercise captured node templates in a generic case
function and a move-only explicit graph case. The original nested reproduction
and its expected output are unchanged.

## Verification of the ownership correction

The Release build completed all configured targets, including Python, web, and
benchmarks, with exit code zero. Both new lifetime regressions pass, including
in the full CTest run. All 2,972 entries ran in 70.93 seconds: 2,970 passed,
two failed, and none were skipped.

The logs are `build/release/ownership-final-build.log`,
`build/release/ownership-focused-switch-tests.log`, and
`build/release/ownership-ctest.log`.

## Nested payload equality correction

The original nested map/switch/`take` test now reaches evaluation and its output
assertion without crashing. Its result contains exactly one valid event at the
first tick and no events at the other five ticks. This matches the expected
root tick schedule.

The first tick prints three branch dictionaries with `direct` values
`k00: 2` through `k04: 6`, matching the expected printed values. The nested
literal equality check returned false because the expected literal had an extra
sequence around its three branch entries. `matches_dictionary()` treats each
top-level element of a TSD tick as a key/value pair. The extra sequence makes
the first element contain three fields, so `scalar_literal::pair()` throws
`invalid_argument`; `matches_sequence()` catches it and returns false.

A probe using the exact original and corrected brace shapes confirmed the
structure: the original first tick has one entry with three fields, while the
corrected first tick has three entries with two fields each. The test removes
one brace pair around the first tick's branch entries. The graph, inputs, keys,
values, and six expected tick positions remain unchanged.

The corrected `tick_selection_test.cpp` translation unit passes all ten test
cases and 36 assertions in a focused Catch runner. It was compiled with headers
from `cbfa8cb6`, matching the existing Release core. An earlier standalone
probe used headers with a different virtual layout and crashed; that was an ABI
mismatch in the probe setup. No production, Python, or web code changed.

Temporary post-run diagnostics recorded the result in
`build/release/ownership-metadata-test.log`. No runtime correction is included.

## Other full-suite failure

`bg_web_python_tests` has seven failing pytest cases and two passing cases.
Nanobind rejects BG native graph/service objects at the web extension boundary
with `incompatible function arguments`. In the latest run,
`_http_handler_input_is_dictionary()` rejects `bg._bg._PythonGraphDefinition`
where it expects `bg::python::PythonGraphDefinition`. This matches the previously
observed binding mismatch. No Python or web code is changed here.

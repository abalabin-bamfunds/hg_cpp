# Map, switch, and reduce first-tick result

Verified on root revision `97dd0e1b` with the nested-output fix `773fee39`.

The original reproductions expected `0` on the first tick, but BG captures
`SKIP`. The populated control matches. `visit_tick` confirmed these are skipped
slots, not invalid ticks. The final BG tests now expect `SKIP` at the first
position; later values are unchanged. The original graph does not crash.

| Input | Expected | Captured |
| --- | --- | --- |
| Empty TSD, then `{1:1}`, `{2:2}`, `{1:2}`, `{2:3}`, remove key 1 | `0,2,5,6,7,4` | `SKIP,2,5,6,7,4` |
| `{1:INVALID}`, then the same remaining ticks | `0,2,5,6,7,4` | `SKIP,2,5,6,7,4` |
| Populated control | `2,5,6,7,4` | `2,5,6,7,4` |

The graph helper and tests are in
[`switch_test.cpp`](../../../tests/evaluation/switch_test.cpp).

- Empty first tick: the source TSD root is valid; the map output root is
  invalid. `NestedReduceRuntime::evaluate` invalidates output for an invalid
  dictionary (`src/reduce_node.cpp:680-687`), so the reduce default is not used.
- Invalid-item first tick: source, map, default, reduce-reference, and
  `mapped[1]` target roots are valid. Both mapped bundle fields have valid
  states and REF runtime types, but their referenced targets are invalid.
  `collect_valid_items` checks the dictionary item's root validity
  (`src/reduce_node.cpp:746-760`), so it includes this singleton instead of
  selecting the default.

Typed bundle construction retains the actual field argument types:
`include/bg/time_series/tsb/tsb.hpp:149` calls
`make_free_time_series_bundle`, which derives fields from those arguments in
`src/time_series/tsb/wiring.cpp:315-337`. Thus a valid bundle root can contain
valid REF handles whose targets are invalid.

The optional field-projection probe's SIGSEGV came from its observer node
template lifetime. The probe declared `const auto observe = node<TraceInputs>()`
inside the graph callback, then returned a call to that template. Wiring retains
node-template references until graph compilation finishes, so the callback
returned while the retained template was still needed. Moving `observe` to
namespace scope removed the SIGSEGV. This follows the
[node-template lifetime contract](../design/graph-wiring.md#wiring-template-call).

That corrected broad probe then aborted with an uncaught
`std::invalid_argument` (`the requested time series representation is not
implemented`), a separate unsupported representation request. A narrower
observer that reads only `mapped[1].as<TSB<SwitchReducePair>>()->a`,
`mapped[1].as<TSB<SwitchReducePair>>()->b`, and
`reduced.as<TSB<SwitchReducePair>>()->b` ran the empty, invalid-item, and
populated sequences without a crash against the matching current library. The
original graph outputs were unchanged. No production change was needed for
this diagnostic crash.

Resolved-storage pointer comparisons across adapters were not used to infer
reduce selection. The raw-reference probe remains an ignored local artifact in
the `nested-reference-output` lane; it was not part of this diagnosis.

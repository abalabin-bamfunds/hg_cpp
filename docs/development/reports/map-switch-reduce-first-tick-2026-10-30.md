# Map, switch, and reduce first-tick result

Verified on root revision `97dd0e1b` with the nested-output fix `773fee39`.

Both reproduction tests expect `0` on the first tick, but BG captures `SKIP`.
The populated control matches. `visit_tick` confirmed these are skipped slots,
not invalid ticks. The original graph does not crash.

| Input | Expected | Captured |
| --- | --- | --- |
| Empty TSD, then `{1:1}`, `{2:2}`, `{1:2}`, `{2:3}`, remove key 1 | `0,2,5,6,7,4` | `SKIP,2,5,6,7,4` |
| `{1:INVALID}`, then the same remaining ticks | `0,2,5,6,7,4` | `SKIP,2,5,6,7,4` |
| Populated control | `2,5,6,7,4` | `2,5,6,7,4` |

The graph helper and tests are in
`.lanes/hgraph-pattern-review/tests/evaluation/switch_test.cpp:828-880`.

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

An optional field-projection observer compiled but exited with SIGSEGV; the
original graph did not. This diagnostic crash is unclassified. Resolved-storage
pointer comparisons across adapters were not used to infer reduce selection.
The raw-reference probe is retained at ignored artifact
`build/release/map_reduce_original_probe.cpp`; run it with
`./build/release/map_reduce_original_probe`. No production code or normal tests
changed.

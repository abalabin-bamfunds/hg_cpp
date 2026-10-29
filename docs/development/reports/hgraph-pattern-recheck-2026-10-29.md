# hgraph pattern recheck

The review lane `agent/hgraph-pattern-review` is rebased onto BG `dfe929f0`.
Seven of the original eleven pattern findings still fail. All seven fail during
wiring; their runtime behavior remains unverified. Four findings now pass.
This recheck changes tests and reports only.

## Resolved findings

| Test | Current result |
| --- | --- |
| `map retargets a selected dictionary reference across a nested graph` | Passes, including held key values after both target changes. |
| `reselecting a rebuilt free list keeps unchanged item references` | Passes with `10, SKIP, SKIP, 20`. |
| `a switch routes a free fixed list between its items and a constant list` | Passes all four expected structural deltas, including held items after activation. |
| `a switch routes a free bundle between its fields and a constant bundle` | Passes all four expected structural deltas, including held fields after activation. |

The last three tests have been promoted from hidden reproductions into the
ordinary suite. The map regression was already an ordinary test. The review's
37 additions now comprise 30 ordinary cases and seven hidden reproductions.

## Remaining pattern failures

| Tests | Cases | Current failure |
| --- | ---: | --- |
| `a switch accepts native and free bundle branches`; `reselecting a rebuilt free bundle keeps unchanged field references` | 2 | `switch case output types are incompatible`. Both fail before evaluation. |
| `take keeps the initial map of branches across nested switch replacement` | 1 | `switch case output types are incompatible` inside `build_take_branch`. The reference/owned TSD branch combination still blocks the cascaded map/switch/take graph. |
| `a nested graph passes a free bundle reference to a value consumer`; `a nested graph passes a free fixed list reference to a value consumer` | 2 | `a time series reference cannot target another reference`. The explicit REF pass-through fails during wiring. |
| `switch-selected mesh branches feed a reduction`; `a reduction observes mesh dependencies after switch outputs change` | 2 | `type variable has no type map entry`, including the existing typed graph boundaries. |

The tests retain their expected output assertions and remain tagged
`[.][repro]`. No production correction was attempted.

## Other suite failures

Both failures from the previous complete main-suite run still reproduce on
`dfe929f0`:

- **Hierarchical scalar conflict test:** the cross-translation-unit helper still
  uses `typeid(bg::Bundle<Instrument>)`, while production uses schema identity.
  The keys differ, so the expected conflict exception is absent.
- **Python web bindings:** `_bg` uses the stable nanobind ABI and `_bg_web` uses
  the regular ABI. Their separate type registries reject wiring-point and graph
  arguments. The web Python suite still has seven failing cases and two passing
  cases, with failures before runtime evaluation.

The prior complete main run had 2,892 passing and two failing CTest entries out
of 2,894. This recheck reran both failed entries; both failed with the same
diagnostics. The results are in the reference-target-identity lane's
`build/review/failure-recheck-console.log`, also copied into this lane's
`build/recheck` directory.

## Scope corrections

The earlier exclusions for `test_reduce_map` and `test_reduce_from_switch` no
longer apply: current BG [reduce](../design/reduce.md) supports non-scalar TSD
items, including TSB items. The ordinary `reduce accepts TSB values` regression
passes in this recheck. The exact upstream compositions remain unverified and
are eligible for porting. Tuple folding still requires a different operation
from BG's TSD reduction. The [review scope table](hgraph-pattern-review-2026-10-28.md#scope-assessment)
records the corrected dispositions.

## Verification

All nine review test translation units were rebuilt with the current headers,
GCC Release flags, and `NDEBUG`. Production headers and source match main.
The executables use lane-local copies of the libraries from the fresh,
fully built `dfe929f0` Release build. Library hashes and compile commands are
recorded in `build/recheck/manifest.json`; `ldd` confirms the lane-local BG
libraries are loaded.

| Run | Cases | Assertions | Result |
| --- | ---: | ---: | --- |
| Ordinary cases in the seven affected engine test files | 217 | 296 | All pass. |
| Ordinary cases in the two affected library test files | 56 | 109 | All pass. |
| Hidden engine reproductions | 6 | 6 | All fail during wiring as recorded above. |
| Hidden library reproduction | 1 | 1 | Fails during wiring as recorded above. |

Run the remaining reproductions explicitly:

```sh
build/recheck/engine-patterns '[repro]'
build/recheck/library-patterns '[repro]'
```

These commands intentionally return a failing status. Ordinary cases run by
omitting the filter. Console logs, JUnit results, build logs, and machine-readable
case results are in `build/recheck`. The original source mappings and historical
results remain in the [initial review](hgraph-pattern-review-2026-10-28.md).

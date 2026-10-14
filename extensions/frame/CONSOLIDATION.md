# Frame lane consolidation

Updated 2026-10-14. The frame implementation lane is based on integrated main
`bf338ca5`. The foundation checkpoint `bd6d9cc5` contains the generic
Row/RowPatch work. Keep `.lanes/frame-field-access` as the active frame lane.

## BG changes

The two BG changes integrated in this task are:

- scalar copy runtime selection, integrated in `a194846e`;
- wiring-time output runtime selection, integrated in `ea8077c2`.

The runtime changes completed at `6051cc63`; its complete C++ and Python suite
passed 1,720/1,720. The frame lane is now rebased onto `bf338ca5`, which also
includes borrowed evaluation inputs. The output hook is available for planned
mixed computed/view frame outputs. Frame output layout specifications remain
extension-owned.

The shared numeric function extraction is held. Frame block arithmetic design
and implementation is next. Whether that work should reuse the held extraction
depends on the design and implementation.

## Archive and lane retirement

[`archive/`](archive/README.md) preserves the original Arrow proposals, probe
source and results, frame-schema benchmark comparisons, active shared-view
sample, and schema offset probe. The nine proposal documents are unchanged and
marked historical. Only the probe README and runnable scripts use paths suited
to their archived location.

An additional recovery archive exists at
`/home/abalabin/_bg/build/frame-lane-recovery/2026-10-14`; it preserves
uncommitted source snapshots, manifests, patches, and unique commit bundles.
The retirement manifest is
`/home/abalabin/_bg/build/frame-lane-recovery/2026-10-14/retirement.json`.
Root retired eight superseded frame checkouts and branches, plus the scalar-copy
and output-runtime integration checkouts and branches. All 11 existing stashes
remain unchanged. `.lanes/frame-field-access` is the only active frame lane.

## Consumer verification

All three standalone consumers were rebuilt against a fresh Debug BG install
of `bf338ca5` at `/home/abalabin/_bg/build/frame-dependency/install`. The Conda
environment is also on `CMAKE_PREFIX_PATH` for unrelated dependencies such as
Arrow. [HANDOFF.md](HANDOFF.md#verification) records build settings, commands,
and current logs. The complete BG suite was not repeated for this revision.

| Consumer | Build and test result |
| --- | --- |
| Arrow enabled, Release | Build passed; 5/5 suites passed |
| Arrow disabled, Release | Build passed; 4/4 suites passed |
| Arrow enabled, Debug with ASan/UBSan | Build passed; 5/5 suites passed |

All builds used `-j12`. The sanitizer test used
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`. CMake resolved
`bg`, `ddvisitor`, and `tpack` from the supplied prefix in every consumer. No
retired-worktree paths occur in the three consumer caches.

Test checkpoint `bf4d9483` exercises borrowed graph input slots with native and
view TSF runtimes. It checks storage identity, nullable cells, and incremental
row and column changes. Frame implementation code required no compatibility
changes.

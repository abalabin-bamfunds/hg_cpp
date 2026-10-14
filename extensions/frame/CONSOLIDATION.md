# Frame lane consolidation

Updated on 2026-10-14. The active frame lane is
`.lanes/frame-field-access`, branch `agent/frame-field-access`, commit
`e65c2cb3`, based on main `052a7f0b`. The lane is clean. Keep it as the sole
active frame lane. Root owns BG integration and lane retirement.

## Current BG scope

Integrate these BG changes onto main:

- scalar copy runtime selection, required by immutable frame snapshots and
  erased Arrow copies;
- wiring-time output runtime selection, required for frame output storage.

Hold the shared numeric function extraction. Do not integrate it with this BG
change or make the frame extension depend on it.
Design frame block mathematics before implementing expression or computed
column nodes. Do not add frame APIs while that design is open.

The frame-specific `Out` specification remains extension-owned. BG does not
define frame output layouts or know frame runtime types.

## Lane and archive disposition

Keep `.lanes/frame-field-access` as the frame implementation lane. Do not
delete any old lane in this task. Root created an additional recovery archive
at `/home/abalabin/_bg/build/frame-lane-recovery/2026-10-14`, including source
snapshots, manifests, patches, and bundles for unique commit series.

The extension archive in [`archive/`](archive/README.md) preserves the original
Arrow proposals, experiment sources and results, frame-schema benchmark
comparisons, the active shared-view benchmark sample, and the schema offset
probe. Proposal documents remain unchanged and are labeled historical. The
probe scripts were updated only for the archive location.

The old frame lanes remain present until root finishes integration and final
consumer verification. Do not remove them from this lane.

## Consumer verification after BG integration

After root provides the integrated main prefix, reconfigure all three
standalone consumers:

| Consumer | Required configuration |
| --- | --- |
| Arrow on | Release, Arrow enabled |
| Arrow off | Release, Arrow disabled |
| Sanitizer | Debug, Arrow enabled, AddressSanitizer and UndefinedBehaviorSanitizer |

Remove every cached BG, `ddvisitor`, and `tpack` path that points into scalar
lanes. Resolve BG and its installed dependencies from the same supplied main
prefix. Build and test with `-j12`. Update [`HANDOFF.md`](HANDOFF.md) with the
actual prefix, configure commands, build results, and test results.

Report a required BG API repair or an unexpected design issue to root. Do not
work around it in the frame extension.

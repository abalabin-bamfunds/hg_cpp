# Frame research archive

This directory preserves design proposals, experiments, and benchmark samples
from earlier frame lanes. These files are historical research, not current API
specifications. Current design and implementation status are in
[`../HANDOFF.md`](../HANDOFF.md) and [`../README.md`](../README.md).

## Arrow and frame proposals

`design/frame_arrow_probe/` contains the nine proposal documents from
`.lanes/frame-arrow-probe/experiments/frame_arrow_probe/`, branch
`agent/frame-arrow-probe`, commit `17153ce220123ba77f092e7e96280ee0fe473adb`.
The documents are preserved verbatim:

- `column_kernel_type_proposal.md`
- `frame_api_review.md`
- `frame_design_note.md`
- `frame_entity_strawman.md`
- `frame_implementation_plan.md`
- `frame_io_api_proposal.md`
- `type_lifetime_api_proposal.md`
- `utility_api_proposal.md`
- `wiring_materialisation_proposal.md`

They capture the design discussion and research that preceded the current
extension. Some proposals were later superseded or implemented in BG. Do not
use them as current API guidance.

`experiments/frame_arrow_probe/` preserves the probe source, Python test and
benchmark, shell scripts, `.gitignore`, README, and measured benchmark report
from the same source lane. The C++ probe exports `sparse_bunch` storage to
PyArrow; its benchmark measures sparse chunk construction, dense gather, and
compute on prebuilt arrays. The result report is unchanged. The README and
benchmark scripts use the archive path, and the scripts resolve the repository
root from their new location. Rerunning them requires the `bg-build` Conda
environment and writes generated files under the ignored `build/` directory.

## Schema and frame benchmarks

`benchmarks/schema-hooks-consumer/frame-schema/` preserves the baseline and
after-storage CSV and Markdown results from the ignored build directory in
`.lanes/frame-schema/extensions/frame/build/schema-hooks-consumer/`. Their
source lane was `agent/frame-schema` at
`1b78c7f9fc95427ddca7b8c873a80e29fc53bdda`. Keep both pairs together: they
record before/after measurements for the same benchmark workload.

`benchmarks/foundation-benchmark/` preserves the active lane's
`shared-view.csv` and `shared-view.md` sample from
`agent/frame-field-access`, commit
`e65c2cb30734b610ab79a0bebbf95281f27091fa`. This is a separate later sample,
not a replacement for the earlier pair.

`experiments/schema_offset_probe/offset_probe.cpp` preserves the standalone
schema layout probe from the ignored `frame-schema` build directory. Its
`FieldProbe` exploration is now covered by BG schema reflection and
`tests/utils/schema_reflection_test.cpp`; the source remains here as historical
research. The generated executable is not retained.

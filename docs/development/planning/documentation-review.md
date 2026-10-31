# Documentation review

The rewrite follows the approved [documentation plan](user-documentation-plan.md).
Examples were checked against engine revision `8246a411`.

## Cycle 1: structure and prerequisites

- Separated graph patterns, plain value nodes, advanced graphs, and advanced nodes.
- Added basic and advanced syntax overviews with links to primary explanations.
- Placed scalar collections before independently updating time series items.
- Grouped construction-time conditions, value selection, filtering, fallbacks,
  alternative publishers, and graph selection in one conditional-pattern chapter.
- Gave iteration a primary chapter covering fixed loops, per-key `map`, shared
  arguments, and instance lifetime.
- Consolidated individual operation pages into chapters with local page contents.

## Cycle 2: API and behavior

- Replaced the previous test runner with the current unified `run_graph` API.
- Documented sparse result storage, dense access, default capture, limits, and the
  difference between typed C++ comparisons and literal comparisons.
- Replaced unsupported scalar item-selection examples with supported queries and
  value-node callbacks. C++ collection callbacks use changing `In<TS<...>>` inputs
  and borrowed scalar views; `Scalar<T>` parameters remain fixed arguments.
- Corrected the guarantee example to supply a const source to a final input.
- Added current text, analytical, tick-selection, construction, accumulation,
  emission, resampling, and runtime-dispatch reference coverage.
- Checked every published runnable pair in Python and C++, including invalidation,
  skipped positions, equal values, collection removal, and instance replacement.

## Cycle 3: language and readability

- Replaced imperative chapter and section titles with topic titles.
- Kept graph construction separate from node evaluation in explanations and examples.
- Distinguished scalar snapshots, collection deltas, input invalidation, and removal.
- Distinguished selected values, merged publishers, and selected graph instances.
- Linked analytical calculations and tick selection from their graph patterns.
- Removed outdated operation-chooser links and fragment-download instructions.
- Started the advanced overview with ordinary explicit time series declarations.
- Kept Python imports and C++ namespace imports direct. Kept build instructions
  inside nested tabs and increased the rendered code font size.

## Cycle 4: publication and verification

- Built the Sphinx site with fresh source processing and warnings treated as errors.
- Corrected chapter anchors and cross-references after the file moves.
- Checked 57 HTML pages and 4,631 local links, including target anchors.
- Checked desktop and mobile layouts, synchronized language tabs, nested build tabs,
  selection persistence, search, Mermaid rendering, and all 200 generated downloads.
- Reviewed representative desktop, mobile, and dark appearance screenshots.
- Ran all 100 Python examples and all 100 compiled C++ examples successfully.
- Built the separate native Python extension example and verified its `[20]` result.
- Passed all 10 example-collection and download-generation tests.

Generated sites, screenshots, extracted programs, and verification logs remain in
ignored build directories. The published site excludes this review, the plan, and
engine design documents.

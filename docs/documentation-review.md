# Documentation review

Scope: the user guide, task guides, API reference, and library reference on
`agent/user-documentation`. Engine design documents remain outside the site.
The initial four review cycles used local `main` at `7e88a8a8`. The lane is now
rebased onto `21a0f87c`. The current pass adds application recipes, concrete
diagnostics, interpreted profiling, and generated downloads. Revision-specific
validation is recorded below.

## Coverage

- Guide: syntax, graph functions, updates, conditionals, update operators, nodes,
  missing values, validity, input views, execution, time, state, lifecycle,
  sources, queues, collections, map/reduce, guarantees, references, switch/mesh,
  generics, variadics, context inputs, application state, and services.
- Tasks: selecting operations, tests, numeric conversion, diagnostics, profiling,
  and native/Python integration.
- Reference: declarations, runtime views, publication, types, scalar schemas,
  literals, testing, execution, state, sources, higher-order graphs, services,
  operator extension, and the complete supplied standard-library surface.
- Python/C++ differences are documented at the point of use. Engine internals
  are linked only in repository authoring material, outside published pages.

## Cycle 1 — structure and prerequisites

Corrections applied:

- Added explicit prerequisite links to all new guide chapters.
- Ordered node value access before optionality, invalidation, and runtime views;
  ordered execution/time before timers, custom scheduling, and external queues.
- Kept current-item aggregation distinct from aggregation over successive updates.
- Kept value selection in Conditionals and graph-instance selection in Switch.
- Added reference links to the established opening chapters and a dedicated
  `const_` entry reachable from the alphabetical index.
- Kept unavailable Python facilities explicit, with supported alternatives in
  the custom reduction, context-input, and application-storage chapters.

Coverage check: every guide topic has a concrete task, a declared prerequisite
path, an example, and a contract or related-task link. The site navigation
contains the complete draft without placeholder pages.

## Cycle 2 — API and behavior

Corrections applied after comparing the full draft with current declarations,
implementation tests, and executable examples:

- Replaced unsupported Python optional input-view and optional return annotations
  with supported plain optional inputs and no-update returns.
- Corrected Python bundle selection to `getattr` with a symbol field name, and
  distinguished graph connections from callback bundle views.
- Corrected queue delivery's first recorded position.
- Replaced unsupported C++ callable context/state parameters with aggregate node
  fields and callbacks. Explained their syntax where first used.
- Corrected state ownership and equal-call wiring reuse; distinguished that reuse
  from value deduplication and documented independent-node options.
- Used graph templates where native result comparisons need literal sentinels;
  documented the distinction from typed optional-scalar comparisons.
- Corrected `run_graph`, scalar fallback, dynamic input activity method names,
  and operator registration names and lifetime. Removed invented Python operator
  decorators and a claimed queue-service adapter API.
- Added explicit `bg::literals` imports and explained the named-argument suffix.
- Incorporated the rebased revision's Boolean collection reductions.
- Corrected native queue scalar construction and supplied concrete result types
  at map, reduce, switch, and service graph boundaries. Kept ordinary dictionary
  selection examples free of unnecessary checked graph wrappers.

The example checker validates published outputs in both languages. The native
extension task is independently configured, built, and called from Python.
Final validation results are recorded below.

## Cycle 3 — readability and consistency

Reviewed every guide, task, and reference page for purpose, prerequisites,
first-use syntax, terminology, duplicated explanations, and documentation tone.

Corrections applied:

- Kept plain calculations before views, standard history before custom state,
  time before timers, and collection shapes before per-key graph instances.
- Replaced lesson-sequence wording in conditionals with direct task links.
- Clarified skipped publication, absent values, validity, and optional parameters
  without treating Python's root `None` recording as an invalidation detector.
- Reduced repeated validity, set-optionality, duration, and event-retention text;
  linked the authoritative entries instead.
- Split long native result checks into a named result and a comparison.
- Added explicit imports to reference fragments and synchronized their language tabs.
- Added the concrete Boolean collection task, including empty-collection behavior.
- Rewrote the internal plan as the implemented concept sequence and language
  capability matrix. Updated stale authoring guidance and verification commands.

The core sequence introduces no custom node, runtime input view, state object,
or collection annotation before the corresponding need. Later chapters state
prerequisites and explain what the additional declaration enables.

## Cycle 4 — publication and retrieval

Corrections applied:

- Checked local HTML anchors as well as Sphinx document references; corrected
  stale optional-set and valid-item fragments and numeric-policy links.
- Updated all ten navigation groups and checked every published page's heading,
  sidebar placement, and language controls.
- Checked synchronized language selection across examples, pages, and reloads.
  Checked that nested C++ build instructions stay hidden until selected.
- Checked desktop and mobile layout, including horizontally scrolling code and
  tables. Inspected rendered guide screenshots.
- Checked search for conditionals, state, Boolean reductions, queues, service
  catalogues, and invalidation. Waited for the complete result list when a query
  matched many pages.
- Compared the library index with all 55 Python operator exports and added
  missing Boolean operations and composition-helper links.
- Added concrete native extension build commands and verified the documented
  module against the installed Python component.

The site remains separate from engine design documentation and uses the existing
Read the Docs configuration. Hosting activation is an external project setting.

## Initial validation

Validated against local `main` at `7e88a8a8` on Linux with Python 3.13.15 and
GCC 14.3.0 (C++23):

- Built and installed the native engine, library, and Python component.
- Executed 55 Python examples and 55 C++ examples; every documented result matched.
- Configured, built, imported, and called the documented native extension; result `[20]`.
- Compiled the reference operator declaration independently.
- Built Sphinx HTML with all warnings treated as errors.
- Checked 74 HTML pages and 6,573 local links, including fragments.
- Checked every published page in desktop and mobile browser layouts, synchronized
  language tabs, hidden C++ setup, navigation, persistence, and representative searches.
- Confirmed index coverage for all 55 Python standard-library operator exports.
- Passed `git diff --check`; changes are confined to documentation and its example checker.

Generated sources, binaries, HTML, browser checks, and screenshots remain in the
lane's ignored `build/` directory. The documentation is committed in its lane;
integration and hosting remain separate owner actions.

## Current pass — worked applications and observability

Rebased onto `main` at `21a0f87c` before editing. The four review cycles below
cover the additions and the APIs changed by that rebase.

1. Structure and prerequisites: added three finite application recipes after
   the guide, linked their prerequisites, and linked them from the relevant
   operation chapters. Kept the plain-type opening sequence unchanged. Added
   a construction/evaluation/runner table after the first custom node.
2. API and behavior: checked actual numeric-overload diagnostics, callback
   exceptions, corrected graphs, profile fields, and retained-period behavior.
   Updated optional Python input views and the `active`, `valid`, and `all_valid`
   decorator options against the rebased implementation and its tests.
3. Readability and consistency: added input/result tables to the recipes,
   explained operation placement through observable behavior, and linked the
   authoritative operation chooser. Removed a redundant selection diagram.
   Distinguished cycle durations, node evaluation counts, and checkpoint ticks.
4. Publication and retrieval: generated Python files and C++ projects from the
   shared checked sources. Replaced catalogue asset links with Sphinx download
   roles after full-site validation found unresolved references. Removed a
   duplicate generated page heading. Checked navigation, language tabs, hidden
   C++ setup, search, diagrams, and desktop/mobile layout. Changed recipe
   diagrams to vertical flow after visual review found small mobile labels.
   Capped inline diagram height while preserving the fullscreen viewer.

Validation against `21a0f87c`, Python 3.13.15, and GCC 14.3.0:

- Refreshed and installed the native engine and Python component.
- All 60 Python examples and 60 C++ examples produce their documented output.
- The shared collector packages those sources as 60 Python files and 60 C++
  project archives. All 60 downloaded C++ sources compile independently. All
  120 published files match their checked sources. Timing/path ellipses require
  an explicit output label.
- All five stdlib tooling tests pass, including adjacent output parsing,
  independent batch output matching, and archive contents.
- Input-property and runner/profiler tests: 38 pass. Six signal tests require
  the optional native test extension, which this documentation build omits;
  they were excluded from the focused run.
- Rebuilt and imported the documented native extension; its graph returns `[20]`.
- Extracted, configured, built, and ran the per-sensor C++ download independently;
  its result check returns `true`.
- Strict Sphinx HTML build passes. Checked 78 HTML pages and 7,447 local links
  and anchors. Browser checks confirm all 120 download links return files.
- All published pages pass desktop/mobile layout and language-tab checks;
  six diagram pages render SVG successfully. Representative searches pass.

Generated downloads, compiled examples, site output, browser checks, and
screenshots remain ignored. The root worktree and its uncommitted changes are
untouched. Integration into the root worktree remains an owner action.

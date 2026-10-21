# Documentation review

Scope: the user guide, task guides, API reference, and library reference on
`agent/user-documentation`. Engine design documents remain outside the site.
The lane starts from local `main` at `86b344b8` and includes the agreed opening
chapters. Review cycles are recorded below after their corrections are applied.

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

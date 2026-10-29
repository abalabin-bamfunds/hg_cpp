# Development documentation

These documents support changes to the BG engine and standard library.
The [project definition](project.md) sets purpose and priorities. The shared
[terminology dictionary](../terminology.md) defines project terms.

For graph and node authoring, see the [user documentation](../user/index.md).

## Document roles

| Location | Purpose |
| --- | --- |
| [User reference](../user/reference/index.md) | Supported interfaces and observable behavior |
| `design/` | Engine structure, internal contracts, ownership, and invariants |
| [Decision records](decisions/) | Reasons for significant choices implemented in committed code |
| [Proposals](proposals/) | Working designs and choices still under discussion |
| [Reports](reports/) | Investigations and framework comparisons |
| [Planning](planning/) | Work plans and review records |

Design documents link to public contracts where they need them. Decision records
link to the current design. Proposal status remains explicit; a proposed choice
requires approval before implementation. Replaced decision records retain their
history and are marked `superseded`.

## Engine design

| Area | Documents |
| --- | --- |
| Nodes and evaluation | [Node API](design/node-api.md), [evaluation](design/evaluation.md) |
| Graph construction | [Graph wiring](design/graph-wiring.md), [variadic wiring](design/variadic-wiring.md), [operators](design/operators.md) |
| Types and scalar values | [Type system](design/type-system.md), [scalar API](design/scalar-api.md) |
| Time series | [Types](design/time-series-types.md), [validity](design/time-series-validity.md), [references](design/time-series-references.md), [providers](design/time-series-providers.md), [dictionary layout](design/tsd-layout.md) |
| Nested graphs | [Boundaries and lifecycle](design/nested-graphs.md), [map](design/map.md), [reduce](design/reduce.md), [mesh](design/mesh.md), [switch](design/switch.md) |
| Shared state and services | [Global state](design/global-state.md), [services](design/services.md) |
| Authoring integration | [Python](design/python.md), [library contracts](design/library.md) |

## Contributor guidance

- [Testing](testing.md): test organization, coverage dimensions, and required checks.
- [Library implementation](library-implementation.md): operator implementation conventions.
- [Documentation maintenance](documentation.md): page structure, examples, tooling, and publication.

## Working designs

- [Node object layout](proposals/node-layout.md): working design with open decisions.
- [C++ tick construction and application](proposals/tick-literals-proposal.md): proposal for discussion.

## Reports

- [hgraph graph-pattern review](reports/hgraph-pattern-review-2026-10-28.md): source patterns, additional tests, and classified findings.
- [hgraph pattern recheck](reports/hgraph-pattern-recheck-2026-10-29.md): remaining failures after the reference identity correction.

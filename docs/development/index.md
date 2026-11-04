# Development documentation

These documents support changes to the HG engine and standard library.
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
- [hgraph pattern recheck, 2026-10-30](reports/hgraph-pattern-recheck-2026-10-30.md): historical `2de7d53f` results and current focused verification after the nested-reference and switch-output corrections.
- [hgraph pattern sweep, 2026-10-30](reports/hgraph-pattern-sweep-2026-10-30.md): additional collection and graph-composition cases with current classifications.
- [hgraph reference-consumer sweep, 2026-10-30](reports/hgraph-reference-consumer-sweep-2026-10-30.md): source switching, mapped references, and higher-order consumers.
- [hgraph reference-consumer sweep, 2026-11-02](reports/hgraph-reference-consumer-sweep-2026-11-02.md): metadata, stateful consumers, free collections, and branch lifetime.
- [hgraph services and feedback sweep, 2026-11-02](reports/hgraph-service-feedback-sweep-2026-11-02.md): nested service clients, delayed collections, and inferred graph outputs.
- [hgraph service composition and scalar feedback sweep, 2026-11-03](reports/hgraph-service-composition-sweep-2026-11-03.md): service chains, recursive requests, multiservice ordering, and hierarchical scalar feedback.
- [hgraph generic services and collection composition sweep, 2026-11-03](reports/hgraph-specialization-collection-sweep-2026-11-03.md): generic service specializations and collection conversion, accumulation, emission, and library reductions.
- [hgraph reference composition sweep, 2026-11-03](reports/hgraph-reference-composition-sweep-2026-11-03.md): additional reference sources, consumers, and nested graph lifetimes.
- [hgraph service queue sweep, 2026-11-03](reports/hgraph-service-queue-sweep-2026-11-03.md): service-owned queue sources and client lifecycle.
- [hgraph reduce composition sweep, 2026-11-03](reports/hgraph-reduce-composition-sweep-2026-11-03.md): higher-order reducer graphs and collection lifecycle.
- [hgraph mesh settling sweep, 2026-11-03](reports/hgraph-mesh-settling-sweep-2026-11-03.md): dependency expansion, retargeting, and branch lifecycle.
- [hgraph timed lifecycle sweep, 2026-11-03](reports/hgraph-timed-lifecycle-sweep-2026-11-03.md): pending work across branch removal and recreation.
- [hgraph dispatch composition sweep, 2026-11-03](reports/hgraph-dispatch-composition-sweep-2026-11-03.md): mapped runtime types, structured outputs, and inherited context.

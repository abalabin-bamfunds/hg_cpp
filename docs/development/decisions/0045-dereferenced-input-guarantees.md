# Dereferenced input guarantees

Status: accepted

## Context

Changing a reference samples its new target. A target's final or const guarantee
constrains that target's updates, but does not constrain how often a mutable
reference can select different targets. Reference deduplication can also publish
distinct references whose targets have equal values.

## Decision

Transparent reference matching limits the target's exposed guarantees using the
[reference guarantee rules](../design/type-system.md#reference-guarantees). Apply the
limit recursively through collections, stopping at the target type of each
contained explicit reference.

Retain original child types while matching collection parameters. Apply an
inherited limit to a value parameter's observed guarantees or to an explicit
reference parameter's reference guarantee. Match that explicit reference's
target without the inherited limit. This is required to create collection
representations without discarding the guarantees of their item targets.

Compute the canonical observed type during signature resolution. Runtime
reference notification and sampling retain their existing behavior and state.

## Consequences

Guarantee variables describe the updates that a consuming input can observe.
Node output propagation uses those guarantees without additional runtime
subscriber state or target-value comparisons. Explicit reference parameters
retain separate reference and target guarantees.

Inner graph inputs preserve supplied reference types. A direct outer graph
output supplies a const reference; a mapped TSD item supplies an ordinary
reference. The [nested graph contract](../design/nested-graphs.md#wiring) defines their
lifetimes.

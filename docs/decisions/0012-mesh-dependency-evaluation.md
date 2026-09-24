# Mesh dependency evaluation

Status: accepted

## Context

A mesh branch can select another branch whose output is not current in the
outer cycle. The selected branch must complete before the selecting node can
publish its output. Runtime keys can change the dependency graph between
cycles.

## Decision

The internal mesh selection node records a pending request in its evaluation
context. Its runtime graph retains the current scheduler position. Nested graph
owners propagate the request through child evaluation contexts until the
selected mesh receives it. The mesh evaluates selected branches iteratively,
then resumes the requesting graph at the same selection node in the same outer
cycle.

Each runtime selection-node instance owns one dependency edge. Mesh retains
these edges across cycles, rejects cycles, and does not evaluate a selected
branch when its output is already current.

## Consequences

Branch graph code remains synchronous: `mesh()[key]` produces a normal time
series input. Public node evaluation functions remain `void`. Pending state is
an internal result between a runtime graph and its owner. It does not add a
mesh-specific capability to the node or runtime graph interfaces.

See [mesh](../mesh.md) and [nested graphs](../nested-graphs.md).

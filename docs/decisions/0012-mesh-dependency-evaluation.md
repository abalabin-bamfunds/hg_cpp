# Mesh dependency evaluation

Status: accepted

## Context

A mesh branch can depend on another branch whose output is not current in the
outer cycle. The dependency branch must complete before the dependency node can
publish its output. Runtime keys can change the dependency graph between cycles.

## Decision

The internal mesh dependency node records its dependency with the mesh and
returns pending when that branch must run. Its runtime graph retains the current
scheduler position. Nested graph owners propagate pending to the mesh. The mesh
evaluates dependency branches iteratively, then resumes each paused graph at
the same dependency node in the same outer cycle.

Each runtime dependency-node instance owns one dependency edge. Mesh retains
these edges across cycles, rejects cycles, and does not evaluate a dependency
branch when its output is already current.

## Consequences

Branch graph code remains synchronous: `mesh()[key]` produces a normal time
series input. Public node evaluation functions remain `void`. Pending state is
an internal result between a runtime graph and its owner. It does not add a
mesh-specific capability to the node or runtime graph interfaces.

See [mesh](../mesh.md) and [nested graphs](../nested-graphs.md).

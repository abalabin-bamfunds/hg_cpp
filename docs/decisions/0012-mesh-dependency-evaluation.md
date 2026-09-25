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
branch when its output is already current. Key invalidation and node stop both
deactivate the edge. The node retains the inactive edge until its runtime
storage is destroyed.

The mesh output item is the persistent reference-valued time series for a
branch. A dependency node dynamically binds an input to that item. When a
branch replaces its output reference, mesh notifies bindings to that output
item before the branch continues. This is required because ordinary output
publication occurs after mesh evaluation completes. The dependency edge stores
only dependency ordering and lifetime state.

## Consequences

Branch graph code remains synchronous: `mesh()[key]` produces a normal time
series input. The dependency node is an aggregate node whose `bool evaluate()`
returns complete or pending. Pending evaluation is a general node result rather
than a mesh-specific node interface.

See [mesh](../mesh.md) and [nested graphs](../nested-graphs.md).

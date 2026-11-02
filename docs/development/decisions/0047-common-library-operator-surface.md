# Common library operator surface

Status: accepted

## Context

Graph authors need one operator API regardless of whether an implementation
belongs to the engine or the standard library. The engine must remain usable
without the library.

## Decision

Expose engine and library operators together through `<hg/library.hpp>` and
`hgraph.operators`. Keep their existing operator identities and normal overload
resolution. Place native library implementations in `hg::library`, which
depends on `hg::hg`; keep the engine independent of that target. Export native
operators to Python through the existing operator wrapper.

## Consequences

Implementation placement determines dependencies, not graph-code syntax. C++
users link `hg::library` for the common surface. Engine-only users can continue
to link `hg::hg`.

See [operators](../design/operators.md#declaration-and-registration) and [Python operators](../design/python.md#graph-authoring).

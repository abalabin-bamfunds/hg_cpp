# Common library operator surface

Status: accepted

## Context

Graph authors need one operator API regardless of whether an implementation
belongs to the engine or the standard library. The engine must remain usable
without the library.

## Decision

Expose engine and library operators together through `<bg/library.hpp>` and
`bg.operators`. Keep their existing operator identities and normal overload
resolution. Place native library implementations in `bg::library`, which
depends on `bg::bg`; keep the engine independent of that target. Export native
operators to Python through the existing operator wrapper.

## Consequences

Implementation placement determines dependencies, not graph-code syntax. C++
users link `bg::library` for the common surface. Engine-only users can continue
to link `bg::bg`.

See [operators](../design/operators.md#declaration-and-registration) and [Python operators](../design/python.md#graph-authoring).

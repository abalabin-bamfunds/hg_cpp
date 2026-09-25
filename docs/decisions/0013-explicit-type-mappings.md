# Explicit type mappings

Status: accepted

## Context

Argument matching cannot resolve a type variable that appears only in an
output. Inferring a positional order from a signature would make small
signature changes alter graph code and would make diagnostics difficult to
interpret.

## Decision

Node, graph, and operator declarations can list ordered type parameters before
their definition or prototype. A call supplies positional arguments through
that declared order or names any type variable directly. The call seeds normal
signature resolution, which checks the mapping against supplied arguments and
uses it to resolve the complete signature.

Indexing a wiring construct returns a proxy that owns the type map. A proxy over
an lvalue wiring construct is reusable. A proxy that owns a temporary wiring
construct is consumed by its wiring call. Operator resolution includes the map
in its cache key.

## Consequences

Existing declarations require no empty parameter list. Named mappings work
without an ordered declaration. Output-only variables can be resolved before
graph code creates a wiring point that depends on them.

See [type system](../type-system.md#signature-resolution),
[graph wiring](../graph-wiring.md#wiring-template-call), and
[operators](../operators.md#declaration-and-registration).

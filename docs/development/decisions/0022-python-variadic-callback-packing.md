# Python variadic callback packing

Status: accepted

## Context

Python graph and node declarations use `*args` and `**kwargs` syntax, but the
function body must receive one TSL or TSB. Calling the original function would
expand that collection according to Python call semantics.

## Decision

Keep the original function and signature as the public wiring interface. Create
an internal callback whose argument slots are positional and whose variadic
flags are cleared. Native wiring passes each captured collection into its
corresponding callback slot.

Graph callbacks receive variadic wiring records. Node callbacks receive
materialized collection input views.

## Consequences

Python call binding and diagnostics retain normal Python signature behavior.
Callback execution receives the same packed collections as C++ variadic graph
and node definitions.

See [Python integration](../design/python.md) and [variadic wiring](../design/variadic-wiring.md).

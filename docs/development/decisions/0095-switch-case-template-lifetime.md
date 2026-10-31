# Switch case template lifetime

Status: accepted

## Context

Case graph templates and graph functions can capture node templates by value.
Wired branches borrow those node templates and use them during structure
comparison and compilation. Destroying a case callable after wiring leaves
the branch with invalid node-template pointers.

## Decision

Retain the case callable used for wiring with the branch wiring through
compilation. This includes the graph template constructed to give a generic
case function a concrete signature.

The type-erased `SwitchCaseArgument` interface continues to borrow graph
templates from its caller under the
[node-template lifetime contract](../design/graph-wiring.md#wiring-template-call).

Ownership belongs to wiring data. Compiled graphs run independently of the
case callables. See [switch arguments](../design/switch.md#arguments).

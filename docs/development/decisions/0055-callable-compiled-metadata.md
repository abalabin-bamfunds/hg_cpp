# Callable compiled metadata

Status: accepted

## Decision

Store invocation parameter descriptors only for definitions that inspect
parameters at runtime. Derive the invocation metadata header requirement from
that need or the presence of immutable compiled data. C++ callable definitions
use compile-time parameter positions and retain only the runtime type pointers
their declared views require.

## Reason

Typed callable argument construction does not inspect parameter names or type
descriptions. Captures have a separate lifetime requirement: their compiled
function object must be destroyed on graph release and compilation failure.
Separating these requirements removes unused shared storage without adding
runtime state or changing parameter-indexed runtime type access.

See [compiled node layout](../proposals/node-layout.md#compiled-node) and
[invocation](../proposals/node-layout.md#invocation).

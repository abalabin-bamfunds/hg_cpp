# Explicit construction of overload registrations

Status: accepted

## Context

Library implementations need adjacent definitions and registrations. Registration
must run after the affected operator objects have been constructed.

## Decision

Group each implementation file's definitions and registration members in an
ordinary class. Library initialization explicitly constructs that class after
constructing the operators. Member initialization order determines registration
order.

## Consequences

Each implementation stays beside its registration without a second list of
implementations. The operator owns the registered templates; the registration
class is temporary and contributes no runtime node storage.

See [overload implementation declarations](../operators.md#overload-implementation-declarations).

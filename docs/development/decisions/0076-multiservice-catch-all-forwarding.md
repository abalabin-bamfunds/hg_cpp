# Multiservice catch-all forwarding

Status: accepted

## Context

A multiservice graph has no outer signature, so the catalogue cannot infer which
service scalar an implementation graph intends to lift into a catch-all request
collection. Ordinary service clients already publish requests through per-client
service bindings and stable client identifiers.

## Decision

Require the multiservice registration and its request/reply ports to name the
lifted scalar explicitly. Keep each original client's request binding and
identifier. Add one forwarding provider per original service instance to copy
its clients' request references and lifted scalars into a shared catch-all
provider, grouped by service, resolved signature, and unlifted scalar values.
Bind the shared reply dictionary to the original service instances by client
identifier.

Exact implementation claims take precedence over catch-all claims. Reject
overlapping catch-all claims only when no exact claim selects the service
instance. An unused catch-all port has an empty request collection and does not
fabricate a scalar value. See [multiservice implementations](../design/services.md#multiservice-implementations).

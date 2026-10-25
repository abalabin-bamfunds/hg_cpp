# Graph services

Status: accepted

## Context

Service clients can be nested at arbitrary graph depth, while service
implementations are selected after ordinary graph wiring. Request delivery must
preserve graph evaluation order without requiring clients to know implementation
locations.

## Decision

Service implementations are wired during catalogue finalization, between root
graph wiring and compilation. Finalization processes newly introduced service
requirements until it reaches a fixed point. Requirements share an implementation
instance only when their service identity, resolved signature, and scalar
arguments match.

Request providers expose their output storage to clients and publish the resulting
changes in the same cycle. Compilation orders each provider after its clients.
Subscription clients require final-guarantee keys and maintain request counts in
`TSD<K, TS<int64_t>>`; its contained key set is the implementation input. Input-view
invalidation withdraws the current request, and a later valid binding can add another
final key. A service provider exposes one
`TSD<service_client_id_t, REF<A>>` bundle field per time-series argument. This
preserves each client's input series without copying its current value. Nested graphs
pass service request bindings through their startup context, while implementation
results use inherited time-series bindings.

Service client identity is a separate const time series so request submission and reply
selection can be wired independently. This allows reply selection to precede request
submission when request values depend on replies, while the implementation response source
remains independent of its request sink.

Service implementations control reply timing. The engine does not defer replies
or re-enter evaluation. A multiservice implementation wires once per build and
associates request and reply ports with its registered service claims.

See [graph services](../design/services.md) for the service contract and runtime details.

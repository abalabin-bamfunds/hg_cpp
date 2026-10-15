# Graph services

This document defines the graph service model and the runtime behavior that service
implementations must preserve.

## Purpose

A service lets graph code exchange time series with one part of the outer graph
without passing wiring points through every enclosing graph. A service declaration
has an operator-like identity and a client-facing signature.

A service implementation does not need to be available when a client is wired. The
root graph wiring collects service requirements, wires matching implementations after
ordinary graph wiring, and then topologically sorts the completed graph.

## Service declarations

`S...` below denotes scalar service parameters. Their order is preserved. Their values
participate in implementation multiplicity.

| Declaration | Client signature | Implementation signature |
| --- | --- | --- |
| Reference | `auto(S...) -> R` | `auto(S...) -> R` |
| Subscription | `auto(TS<K, final>, S...) -> R` | `auto(TSS<K>, S...) -> TSD<K, R>` |
| Service | `auto(A..., S...) -> R` | `auto(TSD<service_client_id_t, A>..., S...) -> TSD<service_client_id_t, R>` |

A reference service exposes one implementation output to any number of clients.

Each subscription call is a distinct runtime client. Its key input must have a final
guarantee. The runtime counts requests for each key and exposes a deduplicated `TSS<K>`
to the implementation. A client observes the output item for its key. A valid existing
item can be sampled immediately. A missing item produces an invalid client result. The
implementation controls when its output ticks; a new request does not imply a
same-cycle reply.

Each service call is a distinct runtime client with a unique `service_client_id_t` identifier. Every client
time-series argument is exposed as a separate dictionary with the same identifier set.
The client observes the corresponding item of the implementation output dictionary.
Subscription services require an output. Outputless clients use a service.

## Implementation multiplicity

One implementation instance serves requirements with the same:

- service identity
- resolved service signature
- scalar service argument values

A generic implementation graph can therefore be wired more than once for different
resolved types or scalar values. More than one implementation candidate can be
installed for a service. A registration can restrict applicability by matching named
service scalar values.

Regular implementation graphs can take installation-specific time-series inputs and
scalars in addition to their service signature. These arguments do not affect service
multiplicity unless they are also service parameters. `register_impl` receives those
installation arguments after the generated service inputs and resolved service scalars.
A named scalar argument whose name matches a scalar service parameter filters the
registration; that filter is not passed again to the implementation graph. Other
arguments are passed to the implementation graph in order. Named arguments must match
the implementation graph's parameter names.

## Wiring and type resolution

A service call resolves its client-facing signature and result type before an
implementation is selected. An unresolved result type is a wiring error unless the
caller supplies the required type mapping explicitly.

Service clients do not assume that an implementation reply is a const reference.
Wiring fixes a reference form with the declared output guarantee before
implementation selection. The selected implementation may supply a direct time
series or a reference. Its output is adapted to the inherited boundary transport
when the implementation is wired. Reference target changes then reach clients
through normal reference notifications, including across nested graphs.

The root `GraphWiring` stores requirements from outer and inner graph wiring.
Finalization groups compatible requirements, selects and wires their implementations,
and repeats until implementation graphs introduce no new requirements. It reports
missing, incompatible, ambiguous, and cyclic requirements with the service call paths
that produced them.

`run_graph` and `test_graph` accept an optional `ServiceCatalogue`. They finalize
services after root wiring and before graph compilation. Code that manages wiring and
compilation directly calls `ServiceCatalogue::build` at the same point.

The compiler topologically sorts the completed graph after service finalization.
Implementation insertion order does not define evaluation order.

## Request delivery

Subscription and service requests are delivered in the same outer cycle as their
clients. Each implementation instance has a generated provider node in the outer
graph. Compilation places that provider after all of its clients.

Each compiled graph assigns dense local service binding indices. At runtime, each binding
points to a `ServiceRequestBinding` owned by the provider node. Nested-graph metadata maps
the local indices to the enclosing graph's indices. The startup context passes the
corresponding bindings through any number of graph boundaries. A generated client retains
its binding during startup and performs no later service lookup. Service
responses use inherited time-series bindings instead.

The provider state constructs an `Out<>` against the provider node's output storage,
runtime type, evaluation context, and graph allocator. Clients mutate that output
directly. The first accepted mutation schedules the provider. Its evaluation function
does no work; normal node-output publication publishes the accumulated changes after
all clients have run. This ordering applies to clients in the root graph and in nested
graphs.

### Subscription clients

A subscription implementation receives the output of one generated request provider:

```text
generated client nodes -> TSD<K, TS<int64_t>> provider output
                                      |
                            contained TSS<K> -> implementation
```

Each service call creates a distinct hidden client node with a required
`TS<K, final>` input. The node has no output and is not deduplicated. Its runtime state
contains the provider-output pointer and the requested key's item identifier. The
service result is wired separately as an ordinary `getitem` of the implementation's
`TSD<K, R>` output.

The client adds its key to the provider dictionary when the input is valid. It removes
that request when the input view is invalidated or the client stops. Each dictionary
item stores the number of clients requesting that key. The client erases the item when
the count reaches zero. The contained `TSS<K>` therefore adds a key on a `0 -> 1`
transition and removes it on a `1 -> 0` transition. Other count changes do not tick the
set. The key value cannot change while the input remains valid because the input has a
final guarantee; invalidation and later rebinding can still withdraw the request and
start another one.

The result `getitem` uses the client's key without feedback delay. A new client can
therefore sample an existing result produced for the same key immediately.

### Service clients

A service call creates a generated client node with the client time-series arguments as
active inputs. It obtains a process-unique identifier and publishes that identifier as
a stable time series value while the client is active. The service result is an ordinary
`getitem` of the implementation output by this identifier.

The provider output is a runtime bundle with one
`TSD<service_client_id_t, REF<A>>` field for each time-series argument. The implementation
receives each field as a named `TSD<service_client_id_t, A>` argument. The argument name
is the service parameter name. While at least one value input is valid, the client keeps
its identifier in every request dictionary. An unbound reference argument has an invalid
item under that identifier. The client removes its identifier from every field when all
value inputs are invalid or when the client stops. Changes to a referenced argument reach
the implementation through normal reference notification; they do not require another
provider-output mutation.

Implementation outputs use ordinary forward dependencies. A reply can reach downstream
clients in the cycle in which the implementation publishes it. Reference services have
no request path. Service delivery does not use pending evaluation or same-cycle
re-entry.

## Multiservice implementations

A multiservice graph wires once and can implement several registered service claims.
Register each service signature and scalar filter that the graph handles:

```cpp
auto registration = catalogue.register_multiservice(implementation_graph);
registration.register_impl(clock);
registration.register_impl(lookup);
registration.register_impl(transform);
```

The catalogue can build independent root graphs repeatedly. Multiservice wiring state
and reply wiring points are scoped to one `build()` call.

Inside the graph, `implementation_requests(...)` provides the request time series
and `implementation_replies(...)` associates an output with a service instance.
Subscription requests are a single `TSS<K>` wiring point. Service requests are
addressable by the declared parameter names because each parameter has its own
`TSD<service_client_id_t, A>` input:

```cpp
auto requests = transform.implementation_requests();
transform.implementation_replies(respond(operators::getattr(requests, "value"_sym)));
lookup.implementation_replies(respond_to(lookup.implementation_requests()));
clock.implementation_replies(clock_output());
```

Reference services have no requests. Outputless services require requests
but no reply. A multiservice graph must return no graph output; attach each service
output through `implementation_replies(...)`. Every used service port must match a
registered claim. A request or reply may be declared only once for a service
instance. Unclaimed, missing, duplicate, and incompatible ports are wiring errors.

A claim can supply explicit type mappings and named scalar filters. The graph may
claim the same service more than once when the filters select different scalar
values. A filter selects which service instances the claim handles; pass the same
scalar arguments to the implementation port to identify the corresponding
instance. Multiservice installation arguments are passed to the graph like regular
implementation arguments.

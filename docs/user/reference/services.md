# Services

Services separate a shared graph contract from its installed implementation. Declare the
contract in graph code and supply a `ServiceCatalogue` to the runner.

## Declarations

| Kind | Python | C++ | Implementation input/output |
| --- | --- | --- | --- |
| Reference | `@reference_service` | `reference_service<Prototype>` | Scalar configuration to shared result |
| Subscription | `@subscription_service` | `subscription_service<Prototype>` | Set of requested keys to dictionary of results |
| Request/reply | `@service` | `service<Prototype>` | Request dictionaries keyed by client id to reply dictionary |

All service parameters need annotations. Python outputs use explicit time series
annotations. C++ prototypes use time series declaration types, not runtime input views.
An outputless request service is a sink; reference and subscription services require
outputs.

Subscription keys require a final-or-stronger guarantee. Their implementation receives
`TSS<K>` and returns `TSD<K, Result>`. Request/reply implementations receive TSD inputs
keyed by service client id and publish replies under those same ids. A client remains
active while any request input is valid; removing it removes its request. Subscription
implementations follow the shared key set.

## Catalogue registration

`catalogue.register_impl(declaration, implementation, ...arguments)` installs an
implementation. Named arguments matching scalar service parameters filter its selection.
Other arguments configure the implementation. Service identity, resolved type mappings,
scalar values, and registration specificity determine matching. Missing or ambiguous
matches are wiring errors.

Python passes `catalogue=catalogue` to the runner. C++ uses `GraphRunOptions{.services =
&catalogue}`. Keep the catalogue and its definitions alive through graph construction
and execution.

## Feedback implementations

`feedback_service_implementation(service, implementation)` wraps a request or
subscription implementation to delay requests by one cycle before delivery. Register the
wrapper with the same filters and installation arguments. The service must accept
requests and have an output; the implementation must have an output. Feedback changes
request delivery; it is not a general permission for same-cycle dependency cycles.

## Multiservice implementations

`register_multiservice(graph)` registers one graph implementing several service claims.
Its registration object's `register_impl(service, ...filters)` adds claims. Inside that
graph, each service exposes `implementation_requests` where it has requests, and
`implementation_replies(output, ...scalar_arguments)` where it has replies. Ports take
all scalar service parameters.

Python multiservice registrations require named scalar filters and do not support
generic service type mappings. C++ also supplies specialized catalogue and catch-all
registration facilities. Use explicit matching scalar arguments for ports; mismatched
claims fail during wiring.

## Queue-backed implementations

A service implementation graph can dispatch requests to an external worker and return a
queue source carrying its replies. The request sink retains the client id; the worker
writes `{client_id: reply}` to a queue declared as `TSD<service_client_id_t, ReplyType>`
in C++. Python can use a queue source with the corresponding integer-keyed reply
dictionary.

Use realtime execution to wait for replies, retain one queue-handle copy per producer,
and stop or join workers through lifecycle cleanup. Queue closure rejects writes before
stop callbacks run. See [sources and queues](sources.md) for writer lifetime and
concurrency rules.

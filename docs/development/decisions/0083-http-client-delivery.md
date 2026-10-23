# HTTP client completion delivery

Status: accepted

## Context

Graph code needs to handle failed HTTP requests without stopping evaluation.
Both client and server adaptors use `HttpResponse` values.

A service client ID survives withdrawal and reactivation. A completion can
already be committed to the engine queue when its request is withdrawn.

## Decision

The client uses libcurl multi to share connection reuse, DNS caching, and
concurrent transfer handling across service clients on one transport thread.

The HTTP client represents local transport failures as `HttpResponse` with
status 599 and an error message in the body. Received HTTP responses retain
their status, headers, and body, including error statuses.

Internal completion messages carry the service client ID and its activation
generation. Graph-side delivery applies withdrawals before completions and
accepts only completions from the current activation.

## Consequences

Graph code handles HTTP responses and transport failures through one time
series type. Status 599 is nonstandard. A server response with status 599 and
a locally generated transport failure have the same representation.

Queued completions from a withdrawn activation cannot reach a later
activation with the same service client ID.

See the [HTTP client API](../../../extensions/web/README.md#client) for request,
service, and lifecycle behavior.

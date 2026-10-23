# HTTP handler authentication

Status: accepted

## Context

Handlers need to select access policies while applications register policy
implementations centrally through the service catalogue.

## Decision

An HTTP handler can select an ordinary request/reply service from request to
`AuthDecision<IDENTITY>`. Acceptance supplies a typed identity to the handler;
rejection supplies an HTTP response. The decision stores two optional fields,
`identity` and `rejection`, with exactly one present.

Separate request and reply maps share const service client IDs. This keeps
request submission upstream of the policy implementation and reply retrieval
downstream.

One projection validates decisions and publishes matching request and identity
dictionaries containing accepted keys only. It retains references to the
original requests.
This is required to prevent pending and rejected requests from starting
per-request handler graphs. Rejection responses join application responses at
the existing HTTP service boundary.

## Consequences

Policy implementations use normal catalogue registration and replacement.
Handlers retain ambient inputs and additional outputs. Transport code does not
interpret credentials or enforce application policies.

See [server authentication](../../../extensions/web/README.md#server-authentication)
for the API and lifecycle.

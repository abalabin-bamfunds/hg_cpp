# Client authentication schemas

Status: accepted

## Context

Client authentication methods require different credential fields. Credential
graphs can change authentication methods as their outputs change.

## Decision

Declare client authentication through an empty `Auth` base schema. Each
supported method has a derived schema containing its required fields. Transport
submission retains an owning base-typed scalar and selects behavior from its
actual derived type. An explicit anonymous value permits anonymous access.
Unsupported auth types produce a local status-599 response.

Keep the server policy interface unchanged. It returns an identity or an HTTP
rejection through `AuthDecision<IDENTITY>`.

## Consequences

The schema identifies the authentication method and its required data. Client
authentication does not require a scheme string or unrelated empty fields.
Derived values retain their type through the existing scalar and queue APIs.

See the [web extension](../../../extensions/web/README.md) for the concrete
schemas and supported transports.

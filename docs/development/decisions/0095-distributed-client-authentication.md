# Distributed client authentication

Status: accepted

## Context

Credential graphs can use ambient graph inputs and client services to obtain
credentials. Authentication selection and demand must therefore run in graph
evaluation, independently of transport installation.

## Decision

One multiservice graph connects auth registration, credential submission, and
the HTTP and WebSocket client services. It derives normalized origins from
client requests, selects the winning registration for each demanded origin,
and feeds selected origins back to registration nodes. Credential graph outputs
are keyed by origin and registration ID. Per-client graphs combine request,
selection, and credential state before submitting through the existing client
update sinks.

HTTP auth waits retain only the latest pending request for each service client.
A replacement request starts its own timeout. The HTTP transport uses a
distinct sequence ID so a late response cannot be delivered after client
removal and recreation.

A WebSocket connect tick replaces the current connection immediately and resets
messages buffered for it. Messages for the new connection wait until its
credentials are ready. Closing while credentials are pending cancels that
connection. Once the handshake is submitted, credential changes do not pause
messages or reconnect the socket.

## Consequences

Applications register credential graphs where their ambient inputs are
available. Credential graphs can use independently authenticatable HTTP
requests to obtain tokens. HTTP and WebSocket clients sharing an origin share
its selected credential graph.

See the [web extension](../../../extensions/web/README.md#client-authentication)
for public APIs and runtime behavior.

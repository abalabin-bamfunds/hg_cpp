# HTTP handler authentication projection

Status: accepted

## Context

Authentication projections must remove keys when requests or decisions become
invalid while retaining references to original requests.

## Decision

Use a generic selector to return a projected decision field or typed `nothing`
according to request validity. A per-request graph reads `identity` or
`rejection` with `getattr` and calls the selector. Map it twice with
`__valid_items__ = true`. Select accepted requests with
`request_dictionary[operators::keys_(identities)]`.

## Consequences

Only accepted requests start handler graphs, and only current rejections reach
the response merge. The accepted request dictionary retains the original
request references. Invalid requests, decisions, and absent fields do not
produce projection keys.

See [server authentication](../../../extensions/web/README.md#server-authentication)
for the API and lifecycle.

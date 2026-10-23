# HTTP handler authentication composition

Status: superseded by [0089](0089-http-handler-authentication-projection.md)

## Context

The authentication projection must remove stale handler and rejection keys when
the request or decision becomes invalid.

## Decision

Use graph composition for the per-request projection. Read `identity` and
`rejection` from each decision with `getattr`; each projection carries the
decision's validity. Use `valid` on the request target to gate both projected
fields. The policy supplies exactly one of identity or rejection as required
by the authentication decision contract.

Gate accepted request references on identity validity. Gate rejection values on
request and decision validity. Materialize compact dictionaries by copying
currently valid items and erasing invalid or removed keys. Only accepted
requests start handler graphs, and only current rejections reach the response
merge.

## Consequences

The policy keeps the separate request and reply maps defined by [0085](0085-http-handler-authentication.md).
The original request references remain the request dictionary values. Request
or decision invalidation removes the corresponding accepted or rejected key.

See [server authentication](../../../extensions/web/README.md#server-authentication)
for the API and lifecycle.

# HTTP client queue delivery

Status: accepted

## Context

The transport worker serializes request, cancellation, and completion events.
The HTTP reply queue is already keyed by the service client ID.

## Decision

Map request inputs to a sink that submits requests and cancellations. Return
the reply queue directly. Keep the activation sequence inside the transport
runtime to associate cancellation with the matching transfer.

The worker processes submitted commands in order. Cancellation removes the
matching in-flight transfers and queues removal of that client's reply before
the worker processes a later activation. A completion written before
cancellation cleanup remains ordered before that cleanup. Graph code creates
separate service clients when each request needs a separate response time
series.

## Consequences

The graph does not filter HTTP replies by activation sequence. Transport queue
ordering defines delivery around withdrawal and reactivation. A completion
already written before cancellation cleanup can be delivered before the
withdrawal takes effect.

See the [HTTP client API](../../../extensions/web/README.md#http-client) for request,
response, and lifecycle behavior.

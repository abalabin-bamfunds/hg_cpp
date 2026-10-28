# Deferred tick logging

Status: accepted

## Context

Quill formats messages on a backend thread. Graph inputs, scalar views, and
format-argument stores borrow data whose lifetime can end before that thread
processes the message. Serialized ticks can own scalar payloads that require
typed destruction.

## Decision

Check the logger's enabled level before capturing data. Capture an owning,
move-only message on the graph thread before reserving Quill queue storage.
Transfer that message with Quill's deferred codec using nonthrowing moves.

Retain ticks in the existing serializer representation together with their
semantic type. Release encoded payloads through `TimeSeriesTickReader`.
Pattern messages own their pattern, names, and scalar values; the backend
constructs the format-argument store and invokes the existing formatters.

Use one process-level Quill logger named `bg`. The core shared library owns
Quill's shared state; other libraries and applications import it. Configuration
uses Quill's API through `bg::logging::logger()`.

See [logging behavior](../design/library.md#text-operations) and
[tick formatting](../design/time-series-types.md#tick-formatting).

## Consequences

Queued messages can outlive graph execution. Serialization failures occur
before Quill reserves queue storage. Formatting failure and message destruction
retain the serializer's existing cleanup behavior. Extension descriptors and
callbacks must remain available until queued messages have been released.

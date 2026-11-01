# BG logging facade

Status: accepted

## Context

Node logging and application startup must use BG APIs so that the logging
backend can change without requiring graph and node code to change.

## Decision

Expose BG severity levels, output configuration, and formatting functions.
Keep Quill headers, types, macros, and target requirements inside the BG build.
Translate BG message-pattern fields at the backend boundary.

Use one owning, move-only deferred formatting callback for both node messages
and the existing [captured tick messages](0083-deferred-tick-logging.md).
Check severity before capturing arguments. Capture string views and C strings
as owning strings. Invoke formatters on the logging thread.

See [logging behavior](../design/library.md#text-operations) and the
[public API](../../user/reference/library-text.md) for the current contract.

## Consequences

Applications configure logging without depending on a backend API. Changing
the backend preserves the public configuration and deferred formatting contract.
Queued values and their formatters must remain usable until message processing
finishes. `flush` waits for queued messages before application cleanup.

# Keyed TSF view mappings

Status: accepted

## Context

Keyed TSF rows use an ordered key tuple to identify a row. A projection must
preserve that identity even when its output schema renames fields.

## Decision

The [TSF view contract](../../README.md#tsf-inputoutput-surface) defines the
required key mapping and validation behavior.

## Consequences

Key mapping validation occurs before output state changes, so a rejected source
cannot replace or invalidate a previously bound view. Computed key fields are
excluded because their values do not come from the source row identity. A full
borrowed key tuple preserves the source schema's uniqueness guarantee through
the field mapping. The view needs neither a row scan nor its own key index.

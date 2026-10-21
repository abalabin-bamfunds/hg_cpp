# Computed TSF fields are selected at wiring

Status: accepted

## Decision

`Out<TSF<Schema>, frame::computed<>>` uses BG's output runtime type hook to
select computed fields from wiring arguments or immutable node configuration.
Without the hook, wiring rejects the declaration. The frame runtime factory
resolves names against the output schema, rejects absent and duplicate names,
and reuses the runtime cache after canonicalizing field order. The output
continues to use the shared runtime-schema view backend. Computed fields accept
writes by name or schema ordinal through the same validation and publication
path as fixed-name outputs.

## Reason

The resolved output schema and computed field selection are both known at
wiring. Selecting the runtime layout there lets generic and concrete schemas
share storage and cache behavior while keeping each node's selection fixed for
the graph lifetime.

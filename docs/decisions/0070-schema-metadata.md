# Shared schema metadata

`Schema` owns immutable metadata as string keys and owned scalar values. The
metadata dictionary participates in canonical schema identity, while dictionary
entry order does not. Values must support reflexive equality; they do not need
hashing because the schema cache hashes metadata names and compares values to
resolve collisions.

Derived schemas inherit base metadata and replace entries with matching names.
Metadata remains local to the schema declaration in the canonical key. Generic
and runtime schema descriptors use the same representation.

This lets extensions attach metadata to shared schemas without adding their
semantics to BG core.

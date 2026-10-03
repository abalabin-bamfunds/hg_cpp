# Provider-owned time-series runtimes

Status: accepted

## Context

Extension libraries can define time-series families whose semantic types and
storage runtimes are not built into BG. A graph output also needs to select
its runtime independently of the representation used by its inputs.

## Decision

Providers define declaration traits, runtime types, and typed input/output
bindings. They register a default runtime for each resolved semantic type.
Output declarations select a default or provider-defined view specification;
runtime selection uses that output declaration and its resolved semantic type.

## Consequences

Provider runtimes and their semantic types must remain alive while the graph can
use them. Output selection does not infer a representation from input runtimes.

See [time-series providers](../time-series-providers.md) and
[scalar APIs](../scalar-api.md).

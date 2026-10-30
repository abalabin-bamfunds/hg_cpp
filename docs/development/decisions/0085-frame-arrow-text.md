# Frame text representation in Arrow

Status: accepted

## Context

BG string columns store shared handles to separate character payloads. Arrow
offset-based string arrays require contiguous character storage. A valid default
BG symbol has no text and remains distinct from a null cell and an empty symbol.

## Decision

Use Arrow string views for borrowed strings so export can describe existing
payloads without packing them. Use large strings for owned exports so copied
values use 64-bit offsets.

Represent symbols with dictionaries. Index validity represents cell validity;
a null dictionary category represents a valid default symbol. This is required
to preserve all three BG values through an Arrow round trip.

Import retains Arrow owners. Symbol imports intern dictionary categories once
and retain stable reader contexts. Scalar batches can read source values
without converting them to native string handles; native snapshots copy those
read values through BG scalar storage traits.

See [Arrow values](../../extensions/frame/README.md#arrow-values) for formats,
limits, and ownership.

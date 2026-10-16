# Literal sentinel identity and tagged calls

Status: accepted

## Decision

Each `Sentinel` is a noncopyable instruction instance. Consumers identify it by
object address. Its symbol is diagnostic. BG and extensions provide singleton
instances with static storage duration.

Calling a sentinel creates a separate `TaggedLiteral` scalar that borrows the
sentinel and argument. Literal consumers inspect tagged calls separately from
bare sentinels. See the [sentinel contract](../scalar-api.md#sentinels).

## Reason

Scalar sentinel values retain pointers to shared instruction instances. Tagged
calls keep argument lifetime and inspection separate from sentinel identity.

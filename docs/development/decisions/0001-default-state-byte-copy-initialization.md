# Default-state byte-copy initialization

Status: accepted

## Context

Graph compilation constructs an initial runtime storage image. Each runtime graph copies this image into its own
storage. Some storage types are not trivially copyable, but their default state has no ownership and supports this
initialization. C++23 cannot express that narrower property.

## Decision

`byte_copy_initializable` accepts a nothrow default-constructible, trivially destructible type when it is trivially
copyable or declares `static constexpr bool supports_byte_copy_initialization = true`.

The declaration applies only to an immediately default-constructed state. It does not permit copying a modified
object by its byte representation.

## Consequences

Runtime storage types can state the required contract without claiming that every value is trivially copyable.
Adding the declaration requires reviewing the type's default state and ownership representation.

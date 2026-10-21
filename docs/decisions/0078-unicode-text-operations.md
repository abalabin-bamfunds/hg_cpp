# Unicode text operations

Status: accepted

## Context

Native string operators must give C++ and Python graph code the same text
semantics. Engine strings store immutable UTF-8 bytes without validating them.
Unicode case mappings can expand text and depend on surrounding code points.

## Decision

Count Unicode code points for text length, indexing, and slicing. Validate the
complete input when an operation interprets code points. Use ICU's root-locale
case mappings and Unicode case folding. Keep byte-preserving operations and
engine string storage unchanged.

Place Unicode support and its ICU dependency in `bg::library`. Return strings
from text-producing symbol operations. This is required to avoid interning
computed text automatically.

## Consequences

Character selection scans the text without a persistent index or per-string
storage. Unicode operations do not require Python or the GIL. ICU's API length
limit applies to case conversion, not to length, indexing, or slicing.

See [text operations](../library.md#text-operations) for the public contract.

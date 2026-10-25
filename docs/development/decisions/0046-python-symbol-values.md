# Python symbol values

Status: accepted

## Context

Python `str` values and engine symbols both contain text, but scalar inference
must distinguish them. Engine symbols are interned for process lifetime, and
the null symbol must remain distinct from empty text.

## Decision

Represent `bg.Symbol` as a non-subclassable `str` subclass. Non-null symbols use
normal string equality and hashing. A null symbol compares equal only to
another null symbol and converts to the empty Python string through `str()`.

The class owns a strong cache keyed by UTF-8 bytes, with a separate key for the
null symbol. Python construction and C++ conversion use this cache so equal
interned symbols reuse one Python object without decoding text on cache hits.

## Consequences

Symbols work with Python string APIs and retain distinct scalar inference from
ordinary `str`. The cache grows with the number of distinct symbols constructed
or converted to Python, matching the process-lifetime C++ symbol interner.

See [Python integration](../design/python.md).

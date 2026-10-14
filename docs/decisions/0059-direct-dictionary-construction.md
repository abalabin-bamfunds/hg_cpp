# Direct dictionary construction

Status: accepted

## Decision

Dictionary runtime construction acquires the final key slot before invoking
the key construction callback. It hashes the stored key and checks for a
duplicate before invoking mapped construction. Storage owners manage slot
commit and cleanup. See the [scalar construction contract](../scalar-api.md#ownership).

## Reason

A recursive key source may have no stored value until its callback runs.
Constructing in the final slot avoids intermediate ownership and a second key
construction. Hashing through the destination runtime supports native,
composed, and provider representations with the same callback interface.

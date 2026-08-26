# TSD layout

Status: working design. Open choices are listed at the end.

## Purpose

This document defines the runtime structure of `TSD<K, TS_T>`, sharing between
its representations, and its tabular buffer view.

## Structure

A `TSD<K, TS_T>` consists of:

- a `TSS<K>` that owns the keys and maps each key to a sparse index
- a `TSL<TS_T>` that owns the item time series at those indices

The set provides added and removed keys. The sparse list provides updated items.
Together they provide the change information of the dictionary.

The set exposes its keys as a list of buffers. Each key buffer has a matching
validity bitset. The sparse list exposes item buffers for the same index ranges
and uses the set's validity bitsets.

For each occupied index, the set also stores whether the corresponding item
time series is valid. A `valid_items` input view uses these bits to select keys
and interpret item validity transitions. The set records indices whose item
validity changes during the current cycle.

## Shared representations

All representations of one source dictionary share key storage and sparse index
assignment. Each representation owns a sparse list with its requested item
type. Representations whose items have the same validity also share the complete
`TSS<K>`.

The source dictionary and its `valid_items` input views use the same `TSS<K>`.
A reference representation can expose different item validity, so it uses
separate item-validity bits.

When the set adds a key, every existing representation constructs an item at the
assigned index. A representation created later constructs an item for each
current key.

The source graph creates representations as described in the
[reference design](time-series-references.md#dynamic-representations). Storage
for a removed key and its items follows the
[outer-cycle lifetime](time-series-references.md#outer-cycle-boundary).

## Nested dictionaries

The structure is recursive. In `TSD<K0, TSD<K1, TS_T>>`, the outer sparse list
contains inner dictionaries. Each inner dictionary has its own `TSS<K1>` and
sparse item list. All representations of one inner dictionary share that inner
key set.

The same rule applies at every nesting level.

## Open implementation choices

1. Sparse index allocation and reuse.
2. Buffer sizes and allocation strategy.
3. Columnar layout of time-series values and last updated times.

# TSD layout

## Purpose

This document defines the runtime structure of `TSD<K, TS_T>` and the storage
shared by its reference representations.

## Structure

A `TSD<K, TS_T>` consists of:

- a `TSS<K>` that owns each key and maps it to a stable item ID
- sparse item storage indexed by those IDs
- an index of items modified in the current cycle

The set provides added and removed keys. The item index provides modified
items. Together they provide the change information of the TSD.

Removing a key keeps its key and item storage until the next outer cycle. A
selection can retain an absent key's item ID. This lets removal notifications
and references use stable addresses for the complete cycle.

## Shared representations

A `TSD<K, REF<TS_T>>` representation of `TSD<K, TS_T>` reads the original key
and item storage. It exposes an item reference for each item and does not copy
item values.

A `TSD<K, TS_T>` representation of a constructed `TSD<K, REF<TS_T>>` also
reads the original key storage. It owns the selected item references,
subscriptions, and change information required to present their targets as its
items. Target values remain in their owning time series.

The graph that owns the referenced TSD creates representations as described in
the [reference design](time-series-references.md#dynamic-representations).
Storage for a removed key and its items follows the
[outer-cycle lifetime](time-series-references.md#outer-cycle-boundary).

## Nested dictionaries

The structure is recursive. In `TSD<K0, TSD<K1, TS_T>>`, the outer item storage
contains inner TSDs. Each inner TSD has its own key set, item IDs, and item
storage. Representations retain this structure at every level.

The same rule applies at every nesting level.

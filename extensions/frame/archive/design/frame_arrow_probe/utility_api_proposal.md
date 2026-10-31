# Utility API proposal for frame storage

Status: implemented and tested in the `frame-utilities` lane; changes are
uncommitted. The focused utility suite passed 16 cases and 557 assertions.
The current root at `6f347969` already supplies `index::first_set(start)`.
This proposal uses the [working frame note](frame_design_note.md) and the
[Arrow probe](README.md). It keeps Arrow-specific objects out of BG utilities.

## Existing operations to use

| Need | Existing operation |
| --- | --- |
| Allocate and reuse logical IDs or physical slots | `occupancy_index::acquire`, `set`, `reset`, `trim` |
| Construct and destroy column cells | `sparse_bunch::emplace_at`, `erase_at` and their runtime-typed callbacks |
| Track row liveness, changes, and column validity | `occupancy_index` and `index<>` |
| Find the next set bit at or after a position | `index::first_set(start)` on dynamic and fixed indexes |
| Release empty value blocks | Automatic on the last `sparse_bunch::erase_at` in a block |

The frame should own the ID-to-slot and slot-to-ID mappings. No general utility
needs to know that an ID differs from a physical position. Compaction must
move columns, validity, and those mappings together, so it belongs to the
frame rather than to one bunch.

## 1. Enumerate live position runs

Reuse the existing starting-position overload:

```cpp
size_type first_set(size_type start) const noexcept;
```

It returns the first set position at or after `start`, or `size()` if none
exists. Dynamic and fixed-size indexes also retain the zero-argument overload.
The dynamic index provides `first_unset(start)`.

Add one operation to `occupancy_index`:

```cpp
template <typename Function>
void for_each_run(Function &&function) const;
```

The callback receives `[first, end)` physical positions with no gaps. The
operation uses word-level `first_set(start)` and `first_unset(start)` searches,
allocates nothing, and requires no mutation of the index during the callback.
An empty index calls the callback zero times. It describes row liveness, not
contiguous storage in any particular column.

This replaces the probe's per-row gap detection and lets the frame count runs
before choosing a chunked export or dense gather. The existing `dense()` and
`extent()` remain useful; no cached run count is proposed.

## 2. Expose physical bunch block boundaries

Add a read-only descriptor to `sparse_bunch<T>`:

```cpp
struct storage_block {
    std::uint64_t first;
    std::uint64_t end;
    const void *data;
};

storage_block block_containing(size_type position) const noexcept;
```

`[first, end)` is the physical position range of the block containing
`position`. `data` points to the start of the allocated block, or is null
when that block has no allocation. The range is returned even for an
unallocated block. Bounds use 64 bits so the exclusive end of the 32-bit
position space is representable. Typed and runtime bunches share the raw
descriptor. A caller may borrow the pointer only while the bunch is
unchanged and alive; a frame view already has the shorter invocation lifetime.

The descriptor reports **raw storage**. It does not claim that every `T` in
the range has been constructed. Returning `std::span<T>` would make that false
claim for nullable columns and removed rows. `details::bunch_blocks` owns the
block calculation; the public method belongs on `sparse_bunch<T>`.

If a frame stores runtime-typed columns, add the matching operation to
`runtime_sparse_bunch`, with its `runtime_item_layout` argument:

```cpp
storage_block block_containing(size_type position,
                               const runtime_item_layout &layout) const noexcept;
```

The adapter can intersect each live run with each column's block ranges. It
then knows exactly where Arrow chunks must split without comparing addresses
of consecutive cells. Columns of different element sizes can have different
block boundaries; the frame adapter must preserve the same row order across
their exported arrays.

## Existing APIs suffice for relocation

For typed columns, the frame can construct a value at a free destination with
`emplace_at`, destroy its source with `erase_at`, update validity with
`index<>::set/reset`, and update occupancy and ID mappings. The runtime bunch
already accepts construction and destruction callbacks. A frame also needs
to preserve previous cells for columns with active consumer subscriptions.
The frame owner can journal those cells or delay reuse of their constructed
storage. The concrete capture representation remains open; bunch utilities
need no knowledge of subscriptions. See the
[previous-cell contract](frame_design_note.md#previous-cells-by-owner-subscription).

A generic `sparse_bunch::move_at` is not proposed. It cannot make a move
across several columns atomic. For columns whose move can throw, the frame
must either build replacement storage before swapping it into use or provide
another explicit rollback strategy. The current scalar runtime type has
`copy_construct` and `destroy`, but no general move operation. A frame-local
column operation may be appropriate if copying runtime-typed columns becomes
a measured compaction cost.

## Validity and export safety

No `index<>` bitmap pointer API is needed for the first exporter. It can
build per-chunk Arrow validity bytes from `contains()`. The inline `index<>`
representation includes metadata, and raw word access would need alignment,
bit-offset, and byte-order rules. If nullable export is costly, a later
general-purpose `copy_bits(first, count, output)` operation could pack a bit
range at word speed without exposing internal storage.

The probe permits an unconstructed null cell inside a shared value block.
Before an Arrow buffer is serialized or sent outside the process, the frame
must ensure that bytes for null slots are initialized or use a sanitized
buffer. A borrowed block descriptor alone does not make those bytes safe to
publish. This policy belongs to the frame's column representation and Arrow
adapter, not to `sparse_bunch` generally.

No Arrow buffer owner, row-ID allocator, special compaction scheduler, or
block-reservation API is proposed for BG utilities at this stage.

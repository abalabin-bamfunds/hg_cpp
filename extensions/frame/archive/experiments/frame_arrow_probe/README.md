# Sparse bunch to Arrow probe

This experiment builds a PyArrow table from `sparse_bunch<std::int64_t>` columns.
It uses `occupancy_index` for live rows and `index<>` for one nullable column.
The design discussion and proposals are archived in
[`../../design/frame_arrow_probe`](../../design/frame_arrow_probe). They record
historical proposals and are not current API documentation.

Run it from the repository root with:

```sh
conda run -n bg-build bash extensions/frame/archive/experiments/frame_arrow_probe/run.sh
```

The test exports each contiguous live value span as a PyArrow chunk. It splits
at deleted row positions and when adjacent physical row values are not adjacent
in memory. It verifies row order, null validity, chunk boundaries, and shared
value buffer addresses.

The measurement at live row 9 is null and is not constructed in its
`sparse_bunch`. The exported chunk for rows 8–10 starts at the constructed
value at row 8 and spans its unconstructed slot at row 9. The probe never
dereferences row 9; it verifies the span using the constructed values at rows 8
and 10. Arrow validity marks the unspecified bytes for row 9 as null.

A second fixture has one contiguous live run at positions 6–9. Positions 6–7
are an unconstructed null-only prefix. Position 7 is the last slot in the
default eight-element first block; positions 8–9 are constructed in the next
block. PyArrow receives two chunks: a synthetic fixed-width values buffer with
an all-null validity bitmap for positions 6–7, then a zero-copy span for
positions 8–9. The table has four rows and two chunks, one more chunk than a
single gathered four-row array. An entirely null live run can use one synthetic
chunk.

The utilities expose values by position but do not expose block spans or a raw
validity bitmap. The probe discovers value spans by comparing addresses from
constructed positions. It builds a per-chunk Arrow validity bitmap from
`index<>`, so the value buffers are shared while the validity bitmap is copied.
The direct span for an interior null works because it lies in the same
allocated block as constructed values on both sides. A null-only prefix has no
BG value address, so the probe adds a synthetic Arrow chunk for that prefix.
The Python view is used only while the C++ storage remains alive. This tests
PyArrow buffer construction, not the Arrow C Data Interface.

## Benchmark

Run the fixed-seed benchmark with:

```sh
conda run -n bg-build bash extensions/frame/archive/experiments/frame_arrow_probe/benchmark.sh
```

It populates 100,000 physical rows for each case, then measures BG copy, sparse
export with and without descriptor-array allocation, and dense export with and
without NumPy destination allocation. The allocation measurements use normal
allocators after warmup, so memory may be reused. It also measures
`pyarrow.compute.sum` and `pyarrow.compute.filter` on arrays built before timing.
Correctness checks compare values, sums, multiplication results, and filtered
results. The command writes the machine details and full results to
[benchmark_results.md](benchmark_results.md).

Sparse export timing includes per-chunk Python wrapper creation. A native C++
adapter could lower that cost. Dense gather does not construct or inspect Arrow
chunks. At 20% dead rows, both patterns contain 80,000 live rows. The current
run measured BG copy alone at 0.196 ms for clustered deletion and 0.192 ms for
scattered deletion. The small difference lies in traversal and copying, and is
sensitive to row layout and measurement conditions. The chunk count is relevant
to sparse export and subsequent Arrow compute.

# Compaction benchmark

Build `bg_frame_compaction_benchmark` with both `BG_FRAME_BUILD_ARROW` and
`BG_FRAME_BUILD_BENCHMARKS` enabled. It writes CSV to standard output; capture
that output when retaining a run.

The native-frame benchmark uses 32,768 initial rows with either one or eight
`i64` columns. It measures no deletion, one deletion at 7/8 of the initial
rows, one adversarial deletion at 3/4, leading and trailing 10% holes, clustered
10% deletion, deletion of 10% followed by addition of 20%, scattered 0.5%, 5%,
and 20% deletion, and fixed-seed random 5% deletion. Each case has five
samples; reported timing is the median across samples. Each timed traversal
runs three times per sample and reports the mean duration. Consumer counts are
1, 2, 5, and 10.

The computed-column cases use 8,192 source rows and two computed `i64` columns
with an independent physical row mapping. They remove every twentieth row and
compare a mapping created in source order with one created in reverse order.
They report physical slot runs, ordinary column batches, aligned two-column
batches, borrowed Arrow chunks, and their traversal/export timings before and
after compacting only the computed storage. The source frame mapping and row
order remain unchanged. These cases measure gap reduction separately from a
physical order mismatch, which compaction preserves.

`native_ms` measures native batch traversal. `borrowed_arrow_sum_destroy_ms`
includes borrowed Arrow export, summing each column with Arrow compute, and
destroying the exported table. `dense_gather_sum_destroy_ms` includes dense
gather-copy export, the same sums, and table destruction. These timings model
one export and sum per consumer. The reported native batch and Arrow chunk
counts describe one traversal or export, regardless of consumer count.
`id_read_ms` measures reads of one column through every 32nd live row ID.

`plan_only_ms` measures the slot movement plan without moving columns or
changing the measured frame. `compact_ms` measures storage compaction. The
compaction allocation counts and bytes use the frame allocator; they exclude
allocations made internally by Arrow. `planned_moves`, `moved_rows`, and
`value_bytes_moved` describe native row relocation work. Computed rows report
their compact time and moved-row count. The benchmark checks sums, row order,
and the unchanged source mapping outside timed regions.

The recorded run in `compaction_benchmark_results.csv` used revision
`cda84197a509a148f7af3c9b71c6bad425ebadd4`, GCC 14.3.0, Arrow 24.0.0, and
Release flags `-O3 -DNDEBUG`. It ran on Linux 6.12.68 on an AMD EPYC 7R13
virtual machine. Timings are environment-specific measurements, not performance
guarantees.

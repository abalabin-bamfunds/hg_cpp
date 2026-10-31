# Range-based compaction comparison

`before.csv` is the previously recorded per-slot compaction baseline described
in `baseline.meta`. `after.csv` measures range relocation at frame revision
`d8dbd8c3`; `after.meta` records its build and runtime details. Both runs use
the same five-sample benchmark, and both report checksum `345655889045100`.

The compared runs have identical non-time outputs: plan checksums, planned and
moved rows, allocated counts and bytes, consumer batch counts, Arrow chunk
counts, and value bytes moved. For `wide_8x_i64`, range relocation reduces
compaction time as follows:

| Deletion pattern | Earlier compaction (ms) | Range compaction (ms) | Moved rows |
| --- | ---: | ---: | ---: |
| Scattered 0.5% | 5.245 | 0.410 | 31,051 |
| Scattered 5% | 5.221 | 0.805 | 30,916 |
| Scattered 20% | 4.484 | 1.886 | 26,162 |

The table below compares consumer time before compaction with the modeled total
after compaction. The after total adds the measured compaction time to the
post-compaction consumer time. Each consumer column is a different workload:

| Deletion pattern | Consumer workload | Before (ms) | After plus compaction (ms) |
| --- | --- | ---: | ---: |
| Scattered 0.5% | 10 native consumers | 2.545 | 2.745 |
| Scattered 0.5% | 10 borrowed Arrow consumers | 5.107 | 1.455 |
| Scattered 0.5% | 10 dense-gather consumers | 20.747 | 20.723 |
| Scattered 5% | 10 native consumers | 3.930 | 3.057 |
| Scattered 5% | 1 borrowed Arrow consumer | 3.999 | 0.915 |
| Scattered 5% | 10 dense-gather consumers | 14.224 | 12.063 |
| Scattered 20% | 10 native consumers | 7.285 | 3.781 |
| Scattered 20% | 1 borrowed Arrow consumer | 16.172 | 1.986 |
| Scattered 20% | 10 dense-gather consumers | 20.202 | 11.365 |

For one Arrow consumer, compare the three available paths directly:

| Deletion pattern | Fragmented borrowed Arrow + sum (ms) | Dense gather + sum (ms) | Compaction + borrowed Arrow + sum (ms) |
| --- | ---: | ---: | ---: |
| Scattered 0.5% | 0.495 | 1.955 | 0.521 |
| Scattered 5% | 3.999 | 1.497 | 0.915 |
| Scattered 20% | 16.172 | 2.026 | 1.986 |

At 20%, compaction plus borrowed Arrow and dense gather are effectively tied
for one consumer. Repeated Arrow consumers make compaction more useful because
the maintenance cost is paid once while each consumer avoids the fragmented
export.

The totals add the medians of separately measured phases; they are not direct
end-to-end measurements. Range relocation makes compaction much cheaper, and
the consumer measurements show when the saved traversal and export work repays
that cost. At 0.5% scattered deletion, native traversal does not repay
compaction in this workload; one Arrow consumer does not repay it, while ten
Arrow consumers do. At 5% and 20%, the modeled savings cover compaction for the
shown workloads.

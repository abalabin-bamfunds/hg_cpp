# Numeric frame and Arrow benchmark

Sample collected on 2026-10-03, AMD EPYC 7R13 (x86_64, KVM), GCC 14.3.0,
Arrow 24.0.0, and committed BG `b5a241f6`. See the
[build instructions](../README.md#performance-sample) and
[CSV](arrow_benchmark_results.csv).

## Findings

- With 14–15 chunks, borrowed Arrow export plus sum was close to native batch
  consumption in this sample.
- Scattered 50% selection produced about 49,000 one-row chunks. Borrowed export
  plus sum took 17–18 ms; dense gather plus sum took about 1.5 ms, and native
  batches took about 1.2 ms.
- The prebuilt dense Arrow kernel took 0.003–0.018 ms. Full view traversal and
  export dominate that kernel cost. Selected traversal still visits source
  rows to test membership; it is a useful next profiling target.
- These results support keeping both explicit export modes. They do not
  establish a deletion-density cutoff. Selection pattern also determines
  fragmentation.
- Borrowed-path timing combines chunk construction, Arrow computation, and
  cleanup. Sum over an already-built fragmented Arrow array was not measured
  separately; these results do not assign the whole overhead to construction.

## Method and measurements

RuntimeGraph source -> filter_greater_than -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. The plain Arrow baseline uses a prebuilt one-chunk array of the final selected values and runs outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. Extension and benchmark code use Release (-O3); installed libbg is Debug. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum ms |
|:---|:---|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.015 [0.015, 0.017] | 0.921 [0.914, 0.930] | 0.873 [0.863, 0.881] | 0.007 [0.006, 0.008] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.015 [0.015, 0.034] | 0.948 [0.939, 0.993] | 0.897 [0.887, 0.915] | 0.008 [0.007, 0.008] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.016 [0.015, 0.016] | 1.116 [1.100, 1.164] | 1.064 [1.050, 1.110] | 0.007 [0.006, 0.007] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.013 [0.013, 0.016] | 1.268 [1.261, 12.676] | 1.226 [1.217, 12.627] | 0.007 [0.007, 0.007] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.019 [0.017, 0.022] | 18.451 [17.360, 53.113] | 18.312 [17.234, 52.954] | 0.008 [0.007, 0.008] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.012 [0.012, 0.013] | 1.515 [1.505, 1.541] | 1.473 [1.465, 1.501] | 0.008 [0.008, 0.009] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.018 [0.017, 0.018] | 0.921 [0.918, 0.955] | 0.879 [0.877, 0.913] | 0.008 [0.008, 0.009] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.017 [0.017, 0.019] | 1.088 [1.027, 1.145] | 1.033 [0.974, 1.082] | 0.008 [0.008, 0.008] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.018 [0.017, 0.032] | 1.019 [1.013, 22.650] | 0.972 [0.967, 22.592] | 0.008 [0.008, 0.009] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.016 [0.015, 0.017] | 1.307 [1.272, 1.388] | 1.254 [1.224, 1.334] | 0.007 [0.007, 0.013] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.049 [0.044, 0.074] | 17.343 [16.969, 51.965] | 17.181 [16.841, 41.552] | 0.008 [0.008, 0.018] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.016 [0.015, 0.018] | 1.527 [1.520, 1.560] | 1.482 [1.475, 1.516] | 0.008 [0.008, 0.009] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.005 [0.005, 0.006] | 1.187 [1.182, 1.210] | 1.160 [1.158, 1.185] | 0.017 [0.017, 0.018] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.005, 0.006] | 1.248 [1.240, 1.278] | 1.214 [1.212, 1.245] | 0.017 [0.016, 0.018] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.005 [0.005, 0.010] | 1.397 [1.368, 1.755] | 1.360 [1.339, 1.671] | 0.017 [0.016, 0.018] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.057 [0.056, 0.059] | 0.814 [0.761, 3.536] | 0.710 [0.669, 3.437] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.058 [0.056, 0.062] | 3.793 [3.628, 25.453] | 3.693 [3.513, 25.350] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.057 [0.055, 0.059] | 0.840 [0.795, 0.851] | 0.738 [0.701, 0.756] | 0.003 [0.003, 0.003] |

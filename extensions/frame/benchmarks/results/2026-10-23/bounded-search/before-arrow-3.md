RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.017] | 0.483 [0.476, 0.517] | 0.097 [0.097, 0.108] | 0.007 [0.006, 0.007] | 0.024 [0.023, 0.026] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.018 [0.017, 0.020] | 0.574 [0.548, 0.610] | 0.180 [0.176, 0.195] | 0.007 [0.007, 0.008] | 0.021 [0.021, 0.024] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.019 [0.018, 0.032] | 0.772 [0.735, 0.847] | 0.372 [0.358, 0.435] | 0.008 [0.007, 0.010] | 0.021 [0.019, 0.026] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.016 [0.015, 0.019] | 1.056 [1.027, 1.164] | 0.675 [0.665, 0.736] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.035] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.025 [0.023, 0.025] | 16.938 [15.417, 17.988] | 16.530 [14.987, 17.505] | 0.007 [0.007, 0.007] | 0.021 [0.020, 0.021] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.052] | 1.419 [1.399, 1.474] | 1.057 [1.042, 1.092] | 0.007 [0.007, 0.008] | 0.021 [0.020, 0.022] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.019 [0.019, 0.022] | 0.566 [0.547, 0.660] | 0.165 [0.161, 0.222] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.021 [0.020, 0.023] | 0.741 [0.661, 0.824] | 0.328 [0.270, 0.390] | 0.007 [0.007, 0.008] | 0.022 [0.021, 0.026] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.022 [0.021, 0.025] | 0.742 [0.729, 0.771] | 0.330 [0.328, 0.352] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.024] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.020] | 1.068 [1.055, 1.104] | 0.675 [0.667, 0.721] | 0.007 [0.007, 0.008] | 0.026 [0.022, 0.031] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.060 [0.054, 0.096] | 16.344 [15.396, 17.215] | 15.878 [14.887, 16.719] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.026] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.017, 0.043] | 1.452 [1.434, 1.472] | 1.056 [1.046, 1.080] | 0.007 [0.007, 0.008] | 0.023 [0.021, 0.028] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.100 [0.098, 0.117] | 0.085 [0.085, 0.103] | 0.016 [0.016, 0.017] | 0.047 [0.046, 0.049] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.233 [0.232, 0.252] | 0.219 [0.218, 0.237] | 0.016 [0.016, 0.017] | 0.047 [0.046, 0.055] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.006] | 0.403 [0.398, 0.444] | 0.389 [0.384, 0.429] | 0.017 [0.016, 0.017] | 0.049 [0.047, 0.061] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.073 [0.072, 0.075] | 0.475 [0.447, 0.513] | 0.129 [0.124, 0.134] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.030] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.071 [0.069, 0.074] | 3.139 [3.023, 3.215] | 2.795 [2.646, 2.906] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.068 [0.067, 0.071] | 0.548 [0.518, 0.569] | 0.205 [0.199, 0.214] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

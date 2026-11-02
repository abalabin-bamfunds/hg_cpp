RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.492 [0.469, 0.527] | 0.098 [0.097, 0.119] | 0.006 [0.006, 0.007] | 0.023 [0.022, 0.024] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.470 [0.451, 0.529] | 0.081 [0.077, 0.089] | 0.007 [0.006, 0.007] | 0.023 [0.023, 0.031] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.018] | 0.755 [0.731, 0.829] | 0.366 [0.357, 0.404] | 0.007 [0.006, 0.007] | 0.022 [0.021, 0.029] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.015, 0.016] | 1.062 [1.028, 1.135] | 0.671 [0.664, 0.732] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.025] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.025 [0.022, 0.026] | 16.460 [16.069, 18.489] | 16.007 [15.668, 18.082] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.025] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.014, 0.015] | 1.445 [1.413, 1.510] | 1.070 [1.055, 1.106] | 0.007 [0.007, 0.007] | 0.034 [0.031, 0.044] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.020] | 0.575 [0.543, 0.601] | 0.170 [0.160, 0.194] | 0.007 [0.007, 0.007] | 0.024 [0.023, 0.026] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.022 [0.020, 0.038] | 0.731 [0.592, 0.978] | 0.278 [0.184, 0.336] | 0.007 [0.006, 0.007] | 0.024 [0.024, 0.027] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.020, 0.022] | 0.752 [0.730, 0.793] | 0.339 [0.336, 0.366] | 0.007 [0.007, 0.007] | 0.026 [0.024, 0.027] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.032] | 1.097 [1.067, 1.184] | 0.684 [0.669, 0.743] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.026] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.056 [0.049, 0.060] | 16.529 [16.061, 17.410] | 16.062 [15.574, 16.902] | 0.007 [0.006, 0.007] | 0.024 [0.023, 0.026] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.017, 0.018] | 1.470 [1.459, 1.503] | 1.076 [1.065, 1.111] | 0.006 [0.006, 0.007] | 0.020 [0.019, 0.020] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.120 [0.098, 0.126] | 0.106 [0.085, 0.113] | 0.016 [0.016, 0.017] | 0.048 [0.047, 0.053] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.039 [0.038, 0.041] | 0.025 [0.024, 0.025] | 0.017 [0.016, 0.019] | 0.048 [0.048, 0.049] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.011] | 0.432 [0.418, 0.699] | 0.417 [0.404, 0.667] | 0.018 [0.017, 0.023] | 0.050 [0.049, 0.054] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.071 [0.069, 0.113] | 0.486 [0.452, 0.605] | 0.130 [0.124, 0.134] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.072 [0.069, 0.101] | 3.215 [3.062, 3.448] | 2.869 [2.700, 3.012] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.066 [0.065, 0.076] | 0.545 [0.522, 0.588] | 0.210 [0.199, 0.234] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

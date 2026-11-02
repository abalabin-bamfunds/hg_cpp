RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.481 [0.473, 0.527] | 0.097 [0.097, 0.115] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.559 [0.553, 0.592] | 0.172 [0.169, 0.206] | 0.008 [0.008, 0.009] | 0.027 [0.022, 0.035] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.018] | 0.746 [0.730, 0.780] | 0.358 [0.349, 0.372] | 0.007 [0.006, 0.007] | 0.022 [0.021, 0.025] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.014, 0.022] | 1.057 [1.030, 1.195] | 0.674 [0.667, 0.736] | 0.009 [0.008, 0.011] | 0.023 [0.022, 0.025] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.022 [0.021, 0.024] | 16.191 [15.529, 17.126] | 15.747 [15.103, 16.701] | 0.008 [0.007, 0.013] | 0.028 [0.025, 0.061] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.014, 0.015] | 1.470 [1.420, 1.601] | 1.077 [1.047, 1.213] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.026] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.028] | 0.610 [0.557, 0.647] | 0.167 [0.160, 0.186] | 0.012 [0.011, 0.013] | 0.034 [0.030, 0.039] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.019, 0.022] | 0.757 [0.672, 0.850] | 0.341 [0.282, 0.430] | 0.007 [0.007, 0.007] | 0.022 [0.022, 0.025] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.021, 0.028] | 0.735 [0.714, 0.765] | 0.329 [0.323, 0.354] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.023] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.016, 0.018] | 1.073 [1.057, 1.122] | 0.677 [0.671, 0.743] | 0.007 [0.007, 0.008] | 0.023 [0.022, 0.026] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.060 [0.053, 0.090] | 17.869 [16.336, 22.081] | 17.386 [15.879, 21.258] | 0.007 [0.007, 0.008] | 0.024 [0.022, 0.038] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.016, 0.020] | 1.461 [1.439, 1.599] | 1.061 [1.046, 1.176] | 0.007 [0.007, 0.007] | 0.022 [0.022, 0.023] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.099 [0.098, 0.132] | 0.085 [0.085, 0.116] | 0.017 [0.017, 0.017] | 0.058 [0.050, 0.065] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.234 [0.233, 0.238] | 0.219 [0.218, 0.222] | 0.017 [0.017, 0.018] | 0.048 [0.047, 0.067] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.007 [0.006, 0.012] | 0.679 [0.652, 0.723] | 0.656 [0.633, 0.700] | 0.018 [0.017, 0.022] | 0.052 [0.049, 0.057] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.067, 0.091] | 0.482 [0.442, 0.612] | 0.131 [0.124, 0.139] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.076 [0.070, 0.106] | 3.180 [3.053, 3.614] | 2.788 [2.689, 3.187] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.068 [0.067, 0.081] | 0.552 [0.526, 0.573] | 0.208 [0.198, 0.233] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

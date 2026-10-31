RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.016, 0.018] | 0.486 [0.481, 0.554] | 0.099 [0.098, 0.100] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.030] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.016, 0.017] | 0.556 [0.549, 0.581] | 0.172 [0.170, 0.194] | 0.007 [0.007, 0.034] | 0.023 [0.022, 0.025] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.051] | 0.753 [0.728, 0.796] | 0.358 [0.355, 0.380] | 0.007 [0.007, 0.027] | 0.021 [0.020, 0.023] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.014, 0.016] | 1.054 [1.031, 1.103] | 0.673 [0.666, 0.723] | 0.007 [0.007, 0.008] | 0.032 [0.027, 0.043] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.024 [0.023, 0.025] | 17.065 [16.508, 17.515] | 16.631 [16.062, 17.083] | 0.007 [0.007, 0.007] | 0.023 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.014, 0.016] | 1.428 [1.412, 1.568] | 1.057 [1.048, 1.204] | 0.007 [0.007, 0.008] | 0.031 [0.024, 0.050] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.022] | 0.570 [0.558, 0.652] | 0.166 [0.162, 0.174] | 0.007 [0.007, 0.008] | 0.025 [0.023, 0.026] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.021 [0.021, 0.023] | 0.760 [0.662, 0.823] | 0.336 [0.270, 0.405] | 0.007 [0.007, 0.007] | 0.026 [0.023, 0.028] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.021, 0.023] | 0.737 [0.723, 0.763] | 0.332 [0.325, 0.351] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.026] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.030] | 1.066 [1.056, 1.136] | 0.675 [0.667, 0.684] | 0.007 [0.007, 0.008] | 0.024 [0.022, 0.025] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.056 [0.051, 0.075] | 16.620 [15.787, 19.607] | 16.167 [15.329, 19.110] | 0.013 [0.012, 0.016] | 0.024 [0.022, 0.025] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.019 [0.018, 0.049] | 1.842 [1.565, 2.021] | 1.313 [1.133, 1.510] | 0.012 [0.012, 0.013] | 0.032 [0.022, 0.058] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.102 [0.099, 0.144] | 0.086 [0.085, 0.129] | 0.024 [0.018, 0.049] | 0.061 [0.052, 0.066] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.011 [0.011, 0.012] | 0.413 [0.404, 0.434] | 0.380 [0.373, 0.400] | 0.026 [0.024, 0.028] | 0.072 [0.064, 0.100] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.010 [0.006, 0.013] | 0.630 [0.408, 0.759] | 0.600 [0.393, 0.723] | 0.020 [0.019, 0.022] | 0.063 [0.050, 0.073] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.070 [0.069, 0.089] | 0.499 [0.461, 0.541] | 0.130 [0.125, 0.141] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.074 [0.069, 0.089] | 3.182 [3.069, 3.499] | 2.820 [2.709, 3.146] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.069 [0.068, 0.086] | 0.565 [0.547, 0.640] | 0.219 [0.201, 0.239] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

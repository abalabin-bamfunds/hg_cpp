RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.018 [0.017, 0.032] | 0.494 [0.480, 0.636] | 0.099 [0.097, 0.139] | 0.007 [0.007, 0.011] | 0.023 [0.022, 0.031] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.468 [0.457, 0.540] | 0.082 [0.077, 0.099] | 0.008 [0.008, 0.008] | 0.025 [0.024, 0.028] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.019] | 0.757 [0.740, 0.837] | 0.366 [0.359, 0.402] | 0.007 [0.007, 0.007] | 0.021 [0.020, 0.023] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.015, 0.016] | 1.054 [1.032, 1.135] | 0.673 [0.665, 0.704] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.026] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.025 [0.023, 0.053] | 16.874 [15.776, 19.386] | 16.444 [15.344, 18.959] | 0.007 [0.007, 0.007] | 0.020 [0.019, 0.021] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.014, 0.015] | 1.463 [1.425, 1.512] | 1.087 [1.053, 1.129] | 0.007 [0.006, 0.007] | 0.028 [0.023, 0.041] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.019 [0.019, 0.020] | 0.569 [0.545, 0.598] | 0.165 [0.159, 0.172] | 0.015 [0.008, 0.025] | 0.035 [0.026, 0.043] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.024 [0.022, 0.030] | 0.668 [0.593, 0.747] | 0.251 [0.190, 0.311] | 0.008 [0.007, 0.009] | 0.028 [0.025, 0.031] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.024 [0.022, 0.040] | 0.753 [0.748, 0.820] | 0.340 [0.338, 0.419] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.045] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.018 [0.017, 0.023] | 1.072 [1.060, 1.140] | 0.675 [0.668, 0.716] | 0.007 [0.007, 0.024] | 0.023 [0.021, 0.024] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.058 [0.052, 0.068] | 16.706 [15.739, 17.524] | 16.233 [15.233, 16.911] | 0.008 [0.007, 0.008] | 0.022 [0.021, 0.023] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.019 [0.017, 0.026] | 1.482 [1.460, 1.580] | 1.071 [1.064, 1.128] | 0.007 [0.007, 0.007] | 0.029 [0.021, 0.039] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.007] | 0.100 [0.099, 0.142] | 0.086 [0.085, 0.127] | 0.018 [0.017, 0.021] | 0.052 [0.049, 0.054] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.050 [0.043, 0.073] | 0.032 [0.028, 0.041] | 0.021 [0.019, 0.026] | 0.065 [0.057, 0.070] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.006] | 0.425 [0.417, 0.448] | 0.411 [0.402, 0.433] | 0.022 [0.020, 0.026] | 0.064 [0.057, 0.070] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.065, 0.088] | 0.484 [0.444, 0.503] | 0.130 [0.124, 0.153] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.085 [0.067, 0.095] | 3.148 [3.073, 3.499] | 2.779 [2.694, 3.139] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.065 [0.065, 0.067] | 0.553 [0.523, 0.640] | 0.210 [0.200, 0.241] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

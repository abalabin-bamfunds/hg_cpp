RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.018 [0.018, 0.027] | 1.332 [1.308, 1.370] | 0.412 [0.404, 0.425] | 0.007 [0.006, 0.008] | 0.024 [0.023, 0.025] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.018 [0.018, 0.019] | 1.412 [1.386, 1.508] | 0.518 [0.508, 0.608] | 0.007 [0.007, 0.008] | 0.026 [0.023, 0.032] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.018 [0.018, 0.020] | 1.539 [1.515, 1.604] | 0.648 [0.636, 0.682] | 0.010 [0.007, 0.012] | 0.041 [0.035, 0.056] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.014, 0.028] | 1.942 [1.899, 2.412] | 0.983 [0.960, 1.358] | 0.008 [0.007, 0.008] | 0.024 [0.024, 0.025] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.023 [0.021, 0.025] | 17.308 [17.104, 18.171] | 16.381 [16.186, 17.211] | 0.008 [0.007, 0.014] | 0.023 [0.022, 0.024] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.014, 0.015] | 2.146 [2.085, 2.210] | 1.230 [1.203, 1.303] | 0.008 [0.007, 0.009] | 0.028 [0.025, 0.035] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.022] | 1.346 [1.328, 1.404] | 0.439 [0.435, 0.473] | 0.007 [0.007, 0.008] | 0.025 [0.024, 0.025] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.020, 0.023] | 1.581 [1.467, 1.675] | 0.632 [0.565, 0.740] | 0.007 [0.007, 0.022] | 0.026 [0.024, 0.054] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.020, 0.029] | 1.480 [1.456, 1.514] | 0.562 [0.554, 0.605] | 0.010 [0.007, 0.011] | 0.036 [0.035, 0.052] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.018 [0.017, 0.022] | 1.884 [1.870, 2.187] | 0.970 [0.965, 1.093] | 0.007 [0.007, 0.008] | 0.022 [0.020, 0.023] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.059 [0.056, 0.082] | 17.340 [17.071, 17.853] | 16.342 [16.073, 16.868] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.025] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.018 [0.017, 0.023] | 2.126 [2.105, 2.192] | 1.210 [1.198, 1.269] | 0.007 [0.007, 0.008] | 0.024 [0.022, 0.027] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.010] | 0.811 [0.810, 0.870] | 0.796 [0.795, 0.843] | 0.019 [0.018, 0.020] | 0.054 [0.051, 0.061] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.007] | 1.011 [0.988, 1.075] | 0.992 [0.972, 1.058] | 0.018 [0.018, 0.031] | 0.052 [0.051, 0.055] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.007] | 1.058 [1.043, 1.130] | 1.036 [1.028, 1.109] | 0.019 [0.018, 0.021] | 0.053 [0.052, 0.055] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.067 [0.066, 0.089] | 0.882 [0.845, 0.922] | 0.187 [0.178, 0.203] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.073 [0.070, 0.092] | 3.670 [3.517, 3.840] | 2.974 [2.847, 3.091] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.006] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.066 [0.065, 0.110] | 0.925 [0.877, 0.982] | 0.237 [0.225, 0.247] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

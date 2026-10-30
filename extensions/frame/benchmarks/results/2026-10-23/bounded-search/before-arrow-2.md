RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.018 [0.017, 0.021] | 0.490 [0.476, 0.568] | 0.099 [0.097, 0.123] | 0.008 [0.007, 0.045] | 0.029 [0.027, 0.035] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.018 [0.017, 0.033] | 0.582 [0.552, 0.970] | 0.191 [0.175, 0.299] | 0.013 [0.013, 0.014] | 0.035 [0.033, 0.083] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.018 [0.017, 0.033] | 0.767 [0.738, 1.191] | 0.367 [0.360, 0.509] | 0.007 [0.007, 0.008] | 0.021 [0.020, 0.022] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.015, 0.019] | 1.055 [1.029, 1.110] | 0.671 [0.665, 0.685] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.024] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.024 [0.023, 0.026] | 16.604 [16.289, 19.823] | 16.176 [15.866, 19.395] | 0.007 [0.007, 0.008] | 0.023 [0.022, 0.024] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.015] | 1.432 [1.408, 1.527] | 1.066 [1.048, 1.107] | 0.007 [0.006, 0.008] | 0.024 [0.022, 0.029] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.021 [0.020, 0.026] | 0.572 [0.559, 0.590] | 0.167 [0.160, 0.186] | 0.007 [0.007, 0.007] | 0.022 [0.022, 0.024] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.023 [0.021, 0.030] | 0.754 [0.677, 1.002] | 0.329 [0.275, 0.502] | 0.007 [0.007, 0.008] | 0.025 [0.024, 0.030] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.024 [0.022, 0.042] | 0.766 [0.722, 1.133] | 0.340 [0.327, 0.502] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.018 [0.017, 0.044] | 1.062 [1.058, 1.137] | 0.674 [0.669, 0.751] | 0.007 [0.007, 0.008] | 0.023 [0.023, 0.023] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.065 [0.060, 0.085] | 17.946 [16.307, 19.436] | 17.396 [15.814, 18.908] | 0.007 [0.007, 0.007] | 0.023 [0.023, 0.026] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.017, 0.054] | 1.467 [1.436, 1.518] | 1.071 [1.046, 1.099] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.035] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.100 [0.099, 0.129] | 0.085 [0.085, 0.107] | 0.019 [0.018, 0.020] | 0.050 [0.048, 0.053] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.235 [0.233, 0.243] | 0.220 [0.219, 0.226] | 0.023 [0.020, 0.056] | 0.047 [0.046, 0.049] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.006] | 0.410 [0.401, 0.426] | 0.396 [0.387, 0.412] | 0.017 [0.017, 0.020] | 0.048 [0.047, 0.078] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.069 [0.069, 0.074] | 0.484 [0.449, 0.536] | 0.133 [0.125, 0.180] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.075 [0.073, 0.107] | 3.232 [3.091, 3.336] | 2.850 [2.728, 2.981] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.068 [0.067, 0.073] | 0.550 [0.528, 0.607] | 0.206 [0.199, 0.218] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

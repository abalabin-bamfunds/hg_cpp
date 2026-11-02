RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.032] | 0.490 [0.466, 0.513] | 0.098 [0.097, 0.116] | 0.007 [0.006, 0.007] | 0.021 [0.020, 0.021] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.017] | 0.462 [0.449, 0.477] | 0.078 [0.075, 0.079] | 0.006 [0.006, 0.006] | 0.021 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.018] | 0.763 [0.736, 0.953] | 0.367 [0.357, 0.405] | 0.006 [0.006, 0.031] | 0.023 [0.022, 0.028] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.014, 0.016] | 1.052 [1.029, 1.064] | 0.674 [0.665, 0.698] | 0.007 [0.007, 0.014] | 0.021 [0.021, 0.022] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.023 [0.021, 0.024] | 18.046 [16.957, 19.496] | 17.641 [16.543, 19.082] | 0.007 [0.006, 0.007] | 0.021 [0.021, 0.028] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.015 [0.014, 0.025] | 1.486 [1.424, 1.560] | 1.104 [1.053, 1.166] | 0.007 [0.007, 0.024] | 0.021 [0.021, 0.022] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.022] | 0.570 [0.554, 0.586] | 0.166 [0.160, 0.172] | 0.007 [0.007, 0.008] | 0.023 [0.021, 0.028] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.021 [0.020, 0.037] | 0.695 [0.602, 0.745] | 0.255 [0.190, 0.326] | 0.007 [0.007, 0.007] | 0.025 [0.021, 0.030] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.022 [0.021, 0.025] | 0.746 [0.723, 0.777] | 0.338 [0.331, 0.365] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.016, 0.020] | 1.071 [1.057, 1.141] | 0.674 [0.668, 0.751] | 0.007 [0.007, 0.008] | 0.022 [0.021, 0.022] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.058 [0.054, 0.072] | 17.921 [17.297, 19.342] | 17.453 [16.817, 18.861] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.024] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.017, 0.037] | 1.494 [1.450, 2.444] | 1.078 [1.056, 1.686] | 0.007 [0.006, 0.007] | 0.021 [0.020, 0.025] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.099 [0.098, 0.115] | 0.085 [0.085, 0.101] | 0.020 [0.017, 0.024] | 0.052 [0.050, 0.061] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.009 [0.006, 0.011] | 0.060 [0.040, 0.070] | 0.034 [0.026, 0.040] | 0.017 [0.016, 0.017] | 0.052 [0.048, 0.062] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.006] | 0.423 [0.419, 0.452] | 0.408 [0.404, 0.437] | 0.019 [0.018, 0.020] | 0.053 [0.050, 0.072] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.067, 0.072] | 0.467 [0.448, 0.518] | 0.131 [0.125, 0.142] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.078 [0.068, 0.089] | 3.505 [3.412, 4.582] | 3.146 [3.014, 4.208] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.065 [0.064, 0.067] | 0.546 [0.535, 0.585] | 0.207 [0.200, 0.222] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

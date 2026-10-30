RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.018 [0.018, 0.020] | 1.307 [1.281, 1.589] | 0.413 [0.404, 0.685] | 0.007 [0.007, 0.007] | 0.022 [0.022, 0.022] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.018 [0.017, 0.019] | 1.409 [1.384, 1.494] | 0.516 [0.508, 0.572] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.018 [0.017, 0.063] | 1.540 [1.509, 1.618] | 0.646 [0.631, 0.707] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.023] | 1.933 [1.895, 2.025] | 0.967 [0.958, 1.017] | 0.007 [0.007, 0.008] | 0.022 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.025 [0.023, 0.034] | 17.996 [17.013, 22.946] | 17.038 [16.070, 21.965] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.015] | 2.113 [2.092, 2.243] | 1.216 [1.202, 1.283] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.019 [0.019, 0.020] | 1.336 [1.325, 1.383] | 0.440 [0.436, 0.454] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.023] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.019, 0.021] | 1.571 [1.518, 1.598] | 0.628 [0.572, 0.693] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.032] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.019 [0.019, 0.040] | 1.501 [1.476, 2.150] | 0.567 [0.555, 0.741] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.034 [0.017, 0.038] | 3.131 [1.867, 3.217] | 1.535 [0.961, 1.583] | 0.007 [0.007, 0.007] | 0.021 [0.020, 0.022] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.061 [0.057, 0.085] | 17.979 [17.013, 19.374] | 16.925 [16.029, 18.336] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.023] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.018 [0.017, 0.039] | 2.168 [2.109, 2.803] | 1.239 [1.206, 1.847] | 0.007 [0.007, 0.010] | 0.026 [0.024, 0.045] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.814 [0.809, 0.868] | 0.798 [0.795, 0.853] | 0.018 [0.017, 0.019] | 0.066 [0.061, 0.106] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.011] | 1.064 [1.015, 1.358] | 1.043 [0.993, 1.322] | 0.017 [0.017, 0.018] | 0.050 [0.049, 0.065] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.007] | 1.098 [1.068, 1.123] | 1.080 [1.049, 1.104] | 0.019 [0.018, 0.041] | 0.052 [0.052, 0.053] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.066, 0.132] | 0.928 [0.852, 0.999] | 0.190 [0.179, 0.262] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.077 [0.068, 0.152] | 4.072 [3.455, 4.879] | 3.240 [2.793, 3.842] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.067 [0.066, 0.069] | 0.946 [0.916, 0.968] | 0.233 [0.227, 0.243] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

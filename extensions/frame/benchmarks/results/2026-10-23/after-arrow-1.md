RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.017] | 0.482 [0.474, 0.499] | 0.098 [0.097, 0.114] | 0.007 [0.006, 0.007] | 0.023 [0.022, 0.025] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.562 [0.554, 0.611] | 0.172 [0.170, 0.228] | 0.007 [0.006, 0.007] | 0.021 [0.021, 0.023] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.018] | 0.749 [0.734, 1.233] | 0.365 [0.351, 0.829] | 0.006 [0.006, 0.007] | 0.022 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.015] | 1.041 [1.032, 1.201] | 0.674 [0.669, 0.693] | 0.007 [0.007, 0.008] | 0.024 [0.023, 0.027] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.024 [0.021, 0.028] | 15.871 [15.526, 17.824] | 15.447 [15.105, 17.425] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.033] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.016] | 1.439 [1.416, 1.618] | 1.058 [1.048, 1.255] | 0.007 [0.007, 0.007] | 0.024 [0.021, 0.025] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.020, 0.023] | 0.577 [0.548, 0.641] | 0.165 [0.161, 0.169] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.024] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.019, 0.023] | 0.759 [0.676, 0.820] | 0.342 [0.282, 0.400] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.023 [0.021, 0.038] | 0.742 [0.723, 0.815] | 0.331 [0.323, 0.365] | 0.007 [0.007, 0.007] | 0.022 [0.022, 0.023] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.020] | 1.067 [1.053, 1.102] | 0.674 [0.667, 0.708] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.039] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.058 [0.055, 0.061] | 16.566 [15.709, 17.774] | 16.084 [15.230, 17.295] | 0.007 [0.007, 0.007] | 0.025 [0.022, 0.027] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.017, 0.018] | 1.465 [1.436, 1.511] | 1.070 [1.051, 1.103] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.029] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.099 [0.098, 0.122] | 0.085 [0.085, 0.108] | 0.018 [0.017, 0.020] | 0.048 [0.047, 0.067] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.233 [0.233, 0.238] | 0.219 [0.218, 0.222] | 0.018 [0.017, 0.019] | 0.050 [0.049, 0.052] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.007 [0.006, 0.008] | 0.693 [0.662, 0.747] | 0.669 [0.643, 0.723] | 0.017 [0.017, 0.018] | 0.049 [0.048, 0.050] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.068, 0.074] | 0.474 [0.447, 0.499] | 0.129 [0.124, 0.135] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.079 [0.072, 0.127] | 3.171 [3.038, 3.278] | 2.840 [2.643, 2.930] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.068 [0.068, 0.069] | 0.550 [0.520, 0.588] | 0.205 [0.198, 0.226] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

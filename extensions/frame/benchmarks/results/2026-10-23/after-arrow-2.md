RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.482 [0.468, 0.509] | 0.098 [0.097, 0.123] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.024] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 15 | 0.017 [0.017, 0.018] | 0.560 [0.550, 0.610] | 0.173 [0.169, 0.178] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.017, 0.017] | 0.745 [0.724, 0.802] | 0.354 [0.347, 0.415] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.015] | 1.054 [1.030, 1.087] | 0.673 [0.666, 0.719] | 0.007 [0.006, 0.007] | 0.023 [0.021, 0.029] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.024 [0.021, 0.025] | 16.225 [15.551, 16.992] | 15.802 [15.152, 16.559] | 0.007 [0.006, 0.007] | 0.021 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.014] | 1.429 [1.407, 1.464] | 1.056 [1.040, 1.099] | 0.007 [0.006, 0.007] | 0.021 [0.021, 0.022] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.019 [0.019, 0.022] | 0.574 [0.559, 0.598] | 0.167 [0.160, 0.181] | 0.007 [0.007, 0.014] | 0.028 [0.023, 0.031] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.019 [0.019, 0.021] | 0.744 [0.657, 0.820] | 0.332 [0.269, 0.399] | 0.007 [0.007, 0.008] | 0.026 [0.024, 0.029] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.021, 0.043] | 0.756 [0.711, 1.251] | 0.330 [0.323, 0.556] | 0.007 [0.007, 0.007] | 0.021 [0.021, 0.022] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.016, 0.028] | 1.102 [1.054, 1.272] | 0.704 [0.670, 0.810] | 0.007 [0.007, 0.013] | 0.029 [0.023, 0.031] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.059 [0.052, 0.065] | 16.596 [15.976, 18.588] | 16.083 [15.488, 18.112] | 0.007 [0.006, 0.007] | 0.023 [0.022, 0.023] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.017 [0.016, 0.018] | 1.462 [1.435, 1.494] | 1.058 [1.046, 1.085] | 0.007 [0.006, 0.007] | 0.022 [0.022, 0.023] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.099 [0.098, 0.123] | 0.085 [0.085, 0.109] | 0.026 [0.023, 0.039] | 0.053 [0.049, 0.056] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.011] | 0.235 [0.234, 0.323] | 0.220 [0.219, 0.307] | 0.019 [0.017, 0.019] | 0.052 [0.050, 0.080] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.007 [0.007, 0.011] | 0.682 [0.654, 0.942] | 0.664 [0.636, 0.910] | 0.018 [0.017, 0.021] | 0.051 [0.049, 0.056] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.069 [0.068, 0.092] | 0.483 [0.440, 0.664] | 0.131 [0.124, 0.202] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.075 [0.069, 0.096] | 3.226 [3.063, 3.668] | 2.828 [2.697, 3.315] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.069 [0.068, 0.123] | 0.555 [0.543, 0.816] | 0.209 [0.198, 0.344] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

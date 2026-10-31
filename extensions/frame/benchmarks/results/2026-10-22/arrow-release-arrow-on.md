RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.019 [0.017, 0.040] | 1.355 [1.324, 2.518] | 0.420 [0.411, 0.769] | 0.008 [0.008, 0.009] | 0.031 [0.027, 0.035] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.018 [0.017, 0.066] | 1.448 [1.416, 2.591] | 0.509 [0.500, 0.992] | 0.007 [0.007, 0.007] | 0.026 [0.025, 0.051] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.019 [0.017, 0.036] | 1.602 [1.540, 2.408] | 0.673 [0.632, 1.162] | 0.007 [0.006, 0.007] | 0.023 [0.023, 0.025] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.016 [0.015, 0.031] | 2.119 [1.945, 3.305] | 1.029 [0.961, 1.567] | 0.007 [0.006, 0.007] | 0.026 [0.024, 0.054] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.027 [0.025, 0.034] | 19.599 [17.647, 21.983] | 18.584 [16.666, 21.011] | 0.007 [0.006, 0.007] | 0.023 [0.022, 0.024] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.016 [0.015, 0.033] | 2.189 [2.108, 4.007] | 1.245 [1.205, 2.263] | 0.007 [0.006, 0.008] | 0.026 [0.025, 0.027] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.026 [0.023, 0.047] | 1.409 [1.363, 2.504] | 0.440 [0.436, 0.798] | 0.012 [0.012, 0.012] | 0.031 [0.030, 0.035] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.021 [0.020, 0.034] | 1.587 [1.490, 1.632] | 0.631 [0.552, 0.699] | 0.007 [0.006, 0.007] | 0.029 [0.025, 0.034] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.021 [0.021, 0.023] | 1.500 [1.478, 1.566] | 0.562 [0.554, 0.582] | 0.007 [0.006, 0.010] | 0.023 [0.021, 0.025] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.027] | 1.907 [1.878, 3.045] | 0.982 [0.960, 1.727] | 0.007 [0.007, 0.007] | 0.027 [0.025, 0.029] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.064 [0.061, 0.118] | 19.040 [17.791, 23.554] | 18.030 [16.781, 22.446] | 0.006 [0.006, 0.007] | 0.023 [0.022, 0.025] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.018 [0.018, 0.041] | 2.175 [2.125, 3.106] | 1.229 [1.206, 2.188] | 0.007 [0.006, 0.007] | 0.026 [0.025, 0.051] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.007 [0.006, 0.012] | 0.965 [0.841, 1.049] | 0.948 [0.824, 1.028] | 0.023 [0.021, 0.025] | 0.060 [0.056, 0.074] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.007] | 0.992 [0.989, 1.054] | 0.976 [0.974, 1.038] | 0.020 [0.018, 0.021] | 0.056 [0.053, 0.060] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.008 [0.007, 0.013] | 1.373 [1.339, 2.438] | 1.347 [1.314, 2.399] | 0.024 [0.021, 0.026] | 0.067 [0.058, 0.081] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.069 [0.068, 0.069] | 0.895 [0.858, 0.936] | 0.185 [0.177, 0.192] | 0.003 [0.003, 0.021] | 0.005 [0.005, 0.006] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.079 [0.073, 0.107] | 3.789 [3.580, 3.904] | 3.056 [2.884, 3.177] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.068 [0.067, 0.069] | 0.953 [0.907, 1.011] | 0.241 [0.227, 0.259] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

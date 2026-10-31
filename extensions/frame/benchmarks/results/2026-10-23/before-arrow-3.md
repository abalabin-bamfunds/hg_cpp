RuntimeGraph source -> filter(selected > 0) -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. Plain Arrow sum uses a prebuilt one-chunk array of final selected values. Plain Arrow build + sum + destroy starts from the pre-existing final_values vector, builds an owning one-chunk array, computes the sum, and destroys the array. Both baselines run outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. When filter membership changes, the implementation rebuilds selected order by scanning the source row set to preserve source order; this is a source-code observation, not phase attribution. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum (prebuilt array) ms | Plain Arrow build + sum + destroy (pre-existing vector) ms |
|:---|:---|---:|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.018 [0.018, 0.021] | 1.394 [1.335, 1.417] | 0.413 [0.407, 0.487] | 0.006 [0.006, 0.007] | 0.021 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.018 [0.018, 0.020] | 1.412 [1.384, 1.508] | 0.519 [0.507, 0.577] | 0.006 [0.006, 0.007] | 0.022 [0.021, 0.022] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.018 [0.018, 0.019] | 1.539 [1.516, 1.666] | 0.652 [0.632, 0.733] | 0.006 [0.006, 0.007] | 0.021 [0.021, 0.023] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.015] | 1.929 [1.901, 1.982] | 0.970 [0.962, 1.005] | 0.007 [0.007, 0.007] | 0.022 [0.021, 0.024] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.024 [0.023, 0.036] | 17.494 [17.236, 17.780] | 16.546 [16.249, 16.842] | 0.007 [0.007, 0.007] | 0.029 [0.024, 0.037] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.026] | 2.111 [2.077, 2.213] | 1.211 [1.190, 1.325] | 0.007 [0.006, 0.007] | 0.022 [0.022, 0.023] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.020 [0.019, 0.021] | 1.348 [1.330, 1.426] | 0.440 [0.436, 0.502] | 0.007 [0.007, 0.007] | 0.023 [0.022, 0.023] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.019, 0.021] | 1.550 [1.493, 1.685] | 0.633 [0.593, 0.724] | 0.007 [0.006, 0.007] | 0.022 [0.022, 0.023] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.019 [0.019, 0.033] | 1.449 [1.444, 1.508] | 0.558 [0.551, 0.576] | 0.007 [0.006, 0.007] | 0.022 [0.022, 0.023] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.017, 0.019] | 1.885 [1.866, 1.976] | 0.971 [0.962, 1.023] | 0.007 [0.006, 0.007] | 0.020 [0.020, 0.021] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.061 [0.057, 0.065] | 17.814 [17.308, 18.798] | 16.821 [16.319, 17.749] | 0.007 [0.007, 0.007] | 0.024 [0.022, 0.030] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.018 [0.017, 0.024] | 2.138 [2.118, 2.276] | 1.223 [1.199, 1.287] | 0.006 [0.006, 0.007] | 0.022 [0.022, 0.024] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 0.810 [0.809, 0.848] | 0.795 [0.795, 0.833] | 0.017 [0.017, 0.018] | 0.053 [0.049, 0.071] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.006] | 1.005 [0.989, 1.068] | 0.989 [0.973, 1.050] | 0.019 [0.017, 0.025] | 0.064 [0.049, 0.083] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.006] | 1.044 [1.040, 1.108] | 1.030 [1.025, 1.091] | 0.017 [0.016, 0.018] | 0.054 [0.049, 0.103] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.066 [0.065, 0.069] | 0.892 [0.861, 0.917] | 0.184 [0.178, 0.192] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.076 [0.069, 0.139] | 3.648 [3.471, 4.130] | 2.956 [2.805, 3.370] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.066 [0.066, 0.091] | 0.953 [0.908, 0.976] | 0.234 [0.225, 0.251] | 0.003 [0.003, 0.003] | 0.005 [0.005, 0.005] |

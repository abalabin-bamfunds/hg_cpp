RuntimeGraph source -> filter_greater_than -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. The plain Arrow baseline uses a prebuilt one-chunk array of the final selected values and runs outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. Extension and benchmark code use Release (-O3); installed libbg is Debug. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum ms |
|:---|:---|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.018 [0.017, 0.032] | 1.048 [0.995, 1.230] | 0.954 [0.917, 1.108] | 0.015 [0.010, 0.027] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.019 [0.018, 0.039] | 1.181 [1.089, 1.435] | 1.041 [0.983, 1.250] | 0.010 [0.009, 0.011] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.018 [0.017, 0.021] | 1.264 [1.249, 1.352] | 1.183 [1.172, 1.268] | 0.009 [0.009, 0.010] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.015 [0.014, 0.016] | 1.390 [1.358, 1.484] | 1.310 [1.290, 1.395] | 0.009 [0.009, 0.012] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.023 [0.021, 0.037] | 21.535 [18.375, 27.118] | 21.296 [18.151, 26.885] | 0.009 [0.008, 0.009] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.013 [0.013, 0.023] | 1.625 [1.568, 1.692] | 1.550 [1.509, 1.628] | 0.016 [0.013, 0.020] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.025 [0.023, 0.047] | 1.036 [1.019, 1.122] | 0.966 [0.948, 1.050] | 0.009 [0.009, 0.023] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.025 [0.023, 0.047] | 1.180 [1.153, 1.232] | 1.092 [1.034, 1.148] | 0.009 [0.009, 0.010] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.026 [0.022, 0.064] | 1.177 [1.123, 1.794] | 1.091 [1.040, 1.622] | 0.009 [0.009, 0.010] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.021 [0.020, 0.045] | 1.464 [1.406, 1.522] | 1.369 [1.325, 1.426] | 0.010 [0.009, 0.010] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.074 [0.066, 0.109] | 26.280 [20.111, 28.848] | 25.970 [19.837, 28.538] | 0.008 [0.008, 0.009] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.020 [0.018, 0.050] | 1.671 [1.633, 2.942] | 1.594 [1.566, 2.766] | 0.008 [0.007, 0.010] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.007 [0.005, 0.014] | 1.397 [1.276, 2.307] | 1.271 [1.234, 2.155] | 0.021 [0.020, 0.023] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.005, 0.011] | 1.367 [1.332, 1.452] | 1.313 [1.290, 1.361] | 0.019 [0.019, 0.025] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.007 [0.006, 0.008] | 1.605 [1.542, 1.724] | 1.527 [1.488, 1.636] | 0.025 [0.023, 0.031] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.068 [0.062, 0.197] | 0.973 [0.886, 1.155] | 0.822 [0.753, 0.919] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.129 [0.098, 0.197] | 4.381 [4.131, 4.694] | 4.078 [3.797, 4.426] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.089 [0.062, 0.181] | 1.067 [0.943, 1.446] | 0.858 [0.805, 1.254] | 0.003 [0.003, 0.003] |

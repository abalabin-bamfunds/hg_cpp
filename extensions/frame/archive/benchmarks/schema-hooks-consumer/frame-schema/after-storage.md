RuntimeGraph source -> filter_greater_than -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. The plain Arrow baseline uses a prebuilt one-chunk array of the final selected values and runs outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. Extension and benchmark code use Release (-O3); installed libbg is Debug. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum ms |
|:---|:---|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.017 [0.016, 0.033] | 0.957 [0.892, 1.541] | 0.900 [0.843, 1.444] | 0.009 [0.009, 0.010] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.017 [0.017, 0.020] | 0.991 [0.937, 1.117] | 0.931 [0.884, 1.031] | 0.008 [0.007, 0.008] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.016, 0.019] | 1.121 [1.099, 1.227] | 1.062 [1.044, 1.149] | 0.008 [0.007, 0.009] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.026] | 1.308 [1.270, 2.262] | 1.256 [1.217, 2.175] | 0.008 [0.008, 0.009] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.026 [0.025, 0.040] | 21.531 [19.005, 24.271] | 21.327 [18.813, 24.060] | 0.007 [0.007, 0.007] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.031] | 1.578 [1.546, 2.751] | 1.527 [1.504, 2.643] | 0.008 [0.007, 0.010] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.027 [0.023, 0.050] | 1.006 [0.966, 1.755] | 0.939 [0.912, 1.650] | 0.008 [0.007, 0.009] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.025 [0.023, 0.047] | 1.145 [1.073, 2.043] | 1.081 [1.002, 1.916] | 0.014 [0.013, 0.016] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.025 [0.023, 0.059] | 1.034 [1.017, 1.861] | 0.980 [0.963, 1.723] | 0.007 [0.007, 0.009] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.018 [0.016, 0.044] | 1.303 [1.286, 1.419] | 1.251 [1.234, 1.360] | 0.008 [0.007, 0.009] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.073 [0.069, 0.123] | 21.747 [19.806, 31.492] | 21.498 [19.486, 31.175] | 0.007 [0.007, 0.008] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.020 [0.018, 0.040] | 1.617 [1.560, 2.777] | 1.555 [1.506, 2.678] | 0.007 [0.007, 0.008] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.005, 0.011] | 1.254 [1.227, 1.825] | 1.208 [1.198, 1.790] | 0.037 [0.029, 0.060] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.005, 0.010] | 1.360 [1.268, 1.541] | 1.313 [1.224, 1.495] | 0.022 [0.020, 0.023] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.005, 0.008] | 1.433 [1.389, 1.683] | 1.373 [1.355, 1.632] | 0.020 [0.019, 0.021] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.067 [0.065, 0.122] | 0.862 [0.781, 1.329] | 0.751 [0.668, 1.146] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.085 [0.071, 0.108] | 3.861 [3.653, 4.172] | 3.704 [3.510, 3.992] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.067 [0.066, 0.103] | 0.966 [0.883, 1.276] | 0.846 [0.777, 1.150] | 0.003 [0.003, 0.003] |

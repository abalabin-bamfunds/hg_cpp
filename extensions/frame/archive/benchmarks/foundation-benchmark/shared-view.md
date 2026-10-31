RuntimeGraph source -> filter_greater_than -> select_column -> sum consumer.
Each case starts with 100,000 all-valid rows. The 50% cases delete 128 rows per cycle (1,664 total); the dense case has no deletions; the 10% scattered case deletes 768 rows per cycle (9,984 total).
Payload updates use IDs below 10,000 at stride 97: 104 selected updates per cycle for clustered 50% and dense selections, 52 for scattered 50%, and 11 for the 10% scattered case.
Graph construction and 100,000-row source seeding are excluded. Two warmup cycles are excluded; eleven steady cycles are measured.
Source mutation time covers frame set/erase calls. Graph-cycle time covers the complete RuntimeGraph evaluation, including source mutation and the consumer. Consumer time covers the selected sum path. The plain Arrow baseline uses a prebuilt one-chunk array of the final selected values and runs outside graph timing. Selected-row and chunk counts are from the final measured cycle. Consumer timing includes Arrow export or dense-gather allocation and destruction.
Native sum and Arrow sum each rescan all selected values; there is no incremental aggregate yet. Extension and benchmark code use Release (-O3); installed libbg is Debug. All times are median [min, max] milliseconds.

| Selection and deletion | Method | Final-cycle selected rows | Final-cycle chunks visited/exported | Source mutation ms | Graph cycle ms | Consumer sum ms | Plain Arrow sum ms |
|:---|:---|---:|---:|---:|---:|---:|---:|
| 50% clustered selection, clustered deletion | native batch sum | 48336 | 14 | 0.017 [0.016, 0.017] | 0.953 [0.942, 0.969] | 0.898 [0.885, 0.909] | 0.011 [0.010, 0.012] |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 48336 | 14 | 0.017 [0.016, 0.026] | 0.967 [0.944, 1.368] | 0.902 [0.884, 1.278] | 0.007 [0.007, 0.007] |
| 50% clustered selection, clustered deletion | dense gather + sum | 48336 | 1 | 0.017 [0.016, 0.018] | 1.116 [1.102, 1.147] | 1.055 [1.040, 1.086] | 0.007 [0.007, 0.007] |
| 50% scattered selection, clustered deletion | native batch sum | 49168 | 49168 | 0.014 [0.014, 0.014] | 1.281 [1.270, 1.316] | 1.228 [1.219, 1.265] | 0.007 [0.007, 0.007] |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.016 [0.015, 0.019] | 15.931 [15.797, 16.246] | 15.857 [15.694, 16.155] | 0.008 [0.007, 0.009] |
| 50% scattered selection, clustered deletion | dense gather + sum | 49168 | 1 | 0.014 [0.013, 0.015] | 1.529 [1.506, 1.547] | 1.483 [1.459, 1.501] | 0.008 [0.007, 0.008] |
| 50% clustered selection, scattered deletion | native batch sum | 49375 | 638 | 0.019 [0.019, 0.020] | 0.980 [0.976, 0.994] | 0.928 [0.926, 0.943] | 0.008 [0.007, 0.008] |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 49375 | 638 | 0.020 [0.020, 0.021] | 1.075 [1.000, 1.132] | 1.003 [0.939, 1.064] | 0.008 [0.007, 0.009] |
| 50% clustered selection, scattered deletion | dense gather + sum | 49375 | 1 | 0.020 [0.019, 0.020] | 1.022 [1.012, 1.062] | 0.964 [0.959, 1.006] | 0.008 [0.007, 0.008] |
| 50% scattered selection, scattered deletion | native batch sum | 49168 | 49168 | 0.017 [0.016, 0.018] | 1.293 [1.285, 1.322] | 1.245 [1.238, 1.274] | 0.008 [0.007, 0.008] |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 | 49168 | 0.036 [0.028, 0.046] | 16.222 [15.813, 16.386] | 16.063 [15.707, 16.269] | 0.008 [0.007, 0.009] |
| 50% scattered selection, scattered deletion | dense gather + sum | 49168 | 1 | 0.018 [0.017, 0.018] | 1.543 [1.535, 1.560] | 1.489 [1.480, 1.506] | 0.008 [0.007, 0.008] |
| 100% selection, no deletion | native batch sum | 100000 | 15 | 0.006 [0.006, 0.006] | 1.338 [1.335, 1.351] | 1.306 [1.305, 1.321] | 0.017 [0.016, 0.017] |
| 100% selection, no deletion | borrowed Arrow + sum | 100000 | 15 | 0.006 [0.006, 0.007] | 1.296 [1.290, 1.325] | 1.255 [1.253, 1.288] | 0.017 [0.017, 0.017] |
| 100% selection, no deletion | dense gather + sum | 100000 | 1 | 0.006 [0.006, 0.007] | 1.506 [1.499, 1.529] | 1.471 [1.463, 1.494] | 0.017 [0.017, 0.017] |
| 10% scattered selection and deletion | native batch sum | 9001 | 9001 | 0.061 [0.060, 0.062] | 0.845 [0.823, 0.877] | 0.743 [0.723, 0.775] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 | 9001 | 0.062 [0.060, 0.063] | 3.581 [3.417, 3.757] | 3.473 [3.308, 3.643] | 0.003 [0.003, 0.003] |
| 10% scattered selection and deletion | dense gather + sum | 9001 | 1 | 0.061 [0.060, 0.063] | 0.899 [0.884, 0.942] | 0.794 [0.780, 0.833] | 0.003 [0.003, 0.003] |

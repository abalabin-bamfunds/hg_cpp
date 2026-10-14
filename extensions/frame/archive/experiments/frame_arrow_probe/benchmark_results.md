# Sparse chunk export benchmark

- Platform: `Linux-6.12.68-92.122.amzn2023.x86_64-x86_64-with-glibc2.34`
- CPU: `AMD EPYC 7R13 Processor` (96 logical CPUs)
- Python: `3.13.15`
- PyArrow: `24.0.0`
- NumPy: `2.5.3`
- Fixed seed: `20260928`
- Physical rows per case: `100,000`
- Warmups: 1 for each sparse export, 5 for BG copy and dense exports, 2 for compute; repetitions: 3 when chunk count is at least 10,000, otherwise 5 for each sparse export; 31 for BG copy and each dense export; 7 for compute
- Exact command: `conda run -n bg-build bash experiments/frame_arrow_probe/benchmark.sh`

Timings are milliseconds as `median [minimum, maximum]` over repeated runs.
Sparse export scans occupied positions, discovers BG spans, and creates PyArrow chunks; its timing includes Python per-chunk wrapper construction. The allocation variant also allocates a zero-filled descriptor array with capacity for every live row.
BG copy visits live positions and copies values into a preallocated NumPy buffer. Dense export adds PyArrow wrapping to that copy. Dense export with allocation creates a fresh NumPy buffer inside each timed call.
Compute inputs are built before timing. The single-chunk baseline is the same live values. A native adapter could reduce sparse export wrapper cost; Arrow compute still sees the resulting chunk count.

| Dead ratio | Pattern | Live | Dead | Live runs | Arrow chunks | Sparse reps | Sparse preallocated | Sparse with allocation | BG copy | Dense preallocated | Dense with allocation | Sum sparse | Sum single | Filter sparse | Filter single |
|---:|:---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.0000% | clustered | 100,000 | 0 | 1 | 15 | 5 | 0.268 [0.267, 0.275] | 0.301 [0.300, 0.312] | 0.237 [0.237, 0.259] | 0.242 [0.241, 0.258] | 0.246 [0.245, 0.271] | 0.022 [0.021, 0.022] | 0.018 [0.017, 0.018] | 0.165 [0.164, 0.172] | 0.157 [0.156, 0.157] |
| 0.0100% | clustered | 99,990 | 10 | 2 | 16 | 5 | 0.268 [0.268, 0.272] | 0.303 [0.301, 0.337] | 0.238 [0.236, 0.244] | 0.242 [0.241, 0.267] | 0.246 [0.244, 0.265] | 0.022 [0.021, 0.022] | 0.019 [0.019, 0.020] | 0.166 [0.165, 0.174] | 0.157 [0.157, 0.158] |
| 0.0100% | scattered | 99,990 | 10 | 11 | 25 | 5 | 0.283 [0.282, 0.323] | 0.318 [0.317, 0.330] | 0.238 [0.237, 0.263] | 0.242 [0.241, 0.261] | 0.245 [0.244, 0.253] | 0.022 [0.021, 0.023] | 0.019 [0.019, 0.019] | 0.168 [0.168, 0.170] | 0.158 [0.157, 0.159] |
| 0.1000% | clustered | 99,900 | 100 | 2 | 16 | 5 | 0.270 [0.269, 0.272] | 0.302 [0.302, 0.303] | 0.237 [0.236, 0.243] | 0.242 [0.240, 0.267] | 0.246 [0.243, 0.303] | 0.021 [0.021, 0.022] | 0.022 [0.019, 0.023] | 0.163 [0.163, 0.173] | 0.157 [0.156, 0.158] |
| 0.1000% | scattered | 99,900 | 100 | 101 | 115 | 5 | 0.417 [0.415, 0.432] | 0.457 [0.456, 0.462] | 0.237 [0.236, 0.243] | 0.242 [0.241, 0.276] | 0.245 [0.244, 0.265] | 0.030 [0.029, 0.031] | 0.019 [0.019, 0.019] | 0.216 [0.213, 0.218] | 0.157 [0.156, 0.157] |
| 1.0000% | clustered | 99,000 | 1,000 | 2 | 16 | 5 | 0.267 [0.266, 0.279] | 0.299 [0.299, 0.300] | 0.235 [0.234, 0.258] | 0.240 [0.238, 0.256] | 0.243 [0.241, 0.284] | 0.021 [0.021, 0.021] | 0.019 [0.018, 0.019] | 0.162 [0.161, 0.163] | 0.155 [0.154, 0.163] |
| 1.0000% | scattered | 99,000 | 1,000 | 988 | 1,002 | 5 | 1.768 [1.760, 1.787] | 1.811 [1.800, 1.847] | 0.235 [0.234, 0.256] | 0.240 [0.238, 0.268] | 0.243 [0.241, 0.263] | 0.113 [0.111, 0.115] | 0.019 [0.018, 0.019] | 0.588 [0.587, 0.595] | 0.155 [0.154, 0.155] |
| 20.0000% | clustered | 80,000 | 20,000 | 2 | 16 | 5 | 0.228 [0.228, 0.234] | 0.255 [0.254, 0.257] | 0.196 [0.195, 0.271] | 0.201 [0.200, 0.217] | 0.204 [0.203, 0.234] | 0.018 [0.017, 0.019] | 0.015 [0.015, 0.015] | 0.135 [0.133, 0.136] | 0.127 [0.126, 0.128] |
| 20.0000% | scattered | 80,000 | 20,000 | 15,950 | 15,960 | 3 | 24.444 [24.431, 24.495] | 24.647 [24.520, 24.898] | 0.192 [0.191, 0.259] | 0.196 [0.195, 0.212] | 0.200 [0.198, 0.208] | 1.506 [1.495, 1.530] | 0.015 [0.015, 0.016] | 5.415 [5.398, 5.438] | 0.126 [0.126, 0.129] |
| 80.0000% | clustered | 20,000 | 80,000 | 2 | 13 | 5 | 0.076 [0.075, 0.078] | 0.083 [0.082, 0.084] | 0.052 [0.052, 0.073] | 0.056 [0.055, 0.068] | 0.059 [0.059, 0.061] | 0.007 [0.006, 0.008] | 0.005 [0.005, 0.006] | 0.039 [0.039, 0.041] | 0.034 [0.034, 0.035] |
| 80.0000% | scattered | 20,000 | 80,000 | 16,003 | 16,003 | 3 | 24.254 [24.202, 24.315] | 24.370 [24.322, 24.377] | 0.057 [0.057, 0.064] | 0.062 [0.061, 0.081] | 0.065 [0.064, 0.070] | 1.403 [1.399, 1.443] | 0.005 [0.005, 0.006] | 5.264 [5.244, 5.326] | 0.034 [0.034, 0.035] |

All cases assert identical values, sums, multiplied values, and filtered outputs before timing.
Chunk count includes splits at deleted-row gaps and BG value-block boundaries.

## Derived one-export plus one-operator comparison

Each total adds the separately measured export and operator medians; the combined operation was not timed directly. The sparse path allocates a descriptor array and the dense path allocates a value array.
| Dead ratio | Pattern | Chunks | Export + sum sparse/dense (ms) | Relative | Export + filter sparse/dense (ms) | Relative |
|---:|:---|---:|---:|---:|---:|---:|
| 0.0000% | clustered | 15 | 0.323 / 0.263 | 1.23x | 0.466 / 0.402 | 1.16x |
| 0.0100% | clustered | 16 | 0.325 / 0.265 | 1.23x | 0.469 / 0.403 | 1.16x |
| 0.0100% | scattered | 25 | 0.340 / 0.264 | 1.29x | 0.486 / 0.403 | 1.21x |
| 0.1000% | clustered | 16 | 0.324 / 0.267 | 1.21x | 0.466 / 0.403 | 1.16x |
| 0.1000% | scattered | 115 | 0.487 / 0.264 | 1.84x | 0.673 / 0.402 | 1.67x |
| 1.0000% | clustered | 16 | 0.321 / 0.261 | 1.23x | 0.462 / 0.398 | 1.16x |
| 1.0000% | scattered | 1,002 | 1.924 / 0.262 | 7.34x | 2.399 / 0.399 | 6.02x |
| 20.0000% | clustered | 16 | 0.273 / 0.219 | 1.24x | 0.390 / 0.331 | 1.18x |
| 20.0000% | scattered | 15,960 | 26.153 / 0.215 | 121.64x | 30.062 / 0.326 | 92.30x |
| 80.0000% | clustered | 13 | 0.090 / 0.065 | 1.38x | 0.122 / 0.094 | 1.30x |
| 80.0000% | scattered | 16,003 | 25.773 / 0.070 | 368.55x | 29.634 / 0.099 | 298.51x |

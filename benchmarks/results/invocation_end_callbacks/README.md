# Invocation-end callback benchmark

## Setup

- Baseline: `fc3b80d11525a7736aa5af1661c9f793d3bbea94`
- Candidate: `74ca1fa921db41f9a7ba49e516feaab720a3725f`
- Both builds: GCC 14.3.0, Release, `build/benchmark`, Linux x86_64.
- Both benchmark targets were built successfully in each tree. The benchmark source is shared by the baseline and candidate.
- Evaluation-context benchmark: 3 runs per build; each evaluation metric reports 9 samples. The table reports the median of each run's median, divided by 200,000 evaluations.
- Runtime scheduling benchmark: 2 baseline runs and 1 candidate run. It reports one timing per workload; dense cost per evaluation is `ns_per_cycle / 32`, and sparse cost is `ns_per_cycle` because one node runs per cycle.

## Results

| Workload | Baseline, ns/evaluation | Candidate, ns/evaluation | Change |
| --- | ---: | ---: | ---: |
| Flat graph evaluation | 105.276 | 110.351 | +4.8% |
| Scalar output evaluation | 139.838 | 129.386 | -7.5% |
| Nested graph evaluation | 156.582 | 165.473 | +5.7% |
| Runtime scheduling, dense 32 nodes | 27.451 | 28.815 | +5.0% |
| Runtime scheduling, sparse 1 node | 68.279 | 76.384 | +11.9% |

The evaluation-context values summarize three runs per build. The runtime scheduling values use the mean of two baseline samples and one candidate sample; treat those as preliminary. Runtime graph size and allocation measurements were unchanged: `sizeof(RuntimeGraph) = 96`, runtime heap bytes were 40 for 32–64 nodes and 64 for 65 nodes.

Across all 27 evaluation samples per build, normalized ranges were:

- Flat graph evaluation: baseline 103.016–108.936 ns/cycle; candidate 108.989–111.313.
- Scalar output evaluation: baseline 138.360–147.468 ns/cycle; candidate 127.711–131.706.
- Nested graph evaluation: baseline 154.370–258.380 ns/cycle; candidate 163.840–173.364. One baseline sample was an outlier at 258.380 ns/cycle; the next-highest baseline sample was 163.256 ns/cycle.

The measurements show lower scalar-output cost and higher flat, nested, and scheduling costs. The flat and nested increases recur across alternating runs. The benchmark does not identify which callback-path operation causes them.

## Runtime scheduling samples

```text
baseline run 1: dense ns_per_cycle=880.854, ns_per_evaluation=27.5267; sparse ns_per_cycle=67.5684, ns_per_evaluation=67.5684
baseline run 2: dense ns_per_cycle=876.016, ns_per_evaluation=27.3755; sparse ns_per_cycle=68.9898, ns_per_evaluation=68.9898
candidate run 1: dense ns_per_cycle=922.080, ns_per_evaluation=28.8150; sparse ns_per_cycle=76.3837, ns_per_evaluation=76.3837
```

## Evaluation-context raw samples

Each value below is a total elapsed time in nanoseconds. Divide by 200,000 to get per-cycle values.

### Baseline run 1

```text
flat_graph_evaluation: 21351075,20709571,21090603,20663459,20734692,20825287,20603196,20708962,20867829
scalar_output_evaluation: 28121892,27829467,28235967,27930733,27967504,27903840,28242658,28060029,27900511
nested_graph_evaluation: 31870360,31377464,31134650,30889808,31688531,31316341,31203404,31420476,30971442
```

### Candidate run 1

```text
flat_graph_evaluation: 21921477,22171400,21977610,22182252,22019633,22192221,22070746,21920407,22070115
scalar_output_evaluation: 25877146,25869636,25695676,25818234,26079445,25951939,25984902,26021503,25614332
nested_graph_evaluation: 32900197,33258815,32836744,32883655,33022113,33094687,33394423,34672772,33690148
```

### Candidate run 2

```text
flat_graph_evaluation: 21846713,22262613,21900726,21995309,21797710,22242444,21842581,21830082,21933166
scalar_output_evaluation: 25712077,25892775,25798151,26027741,25621272,25609550,25542127,25698425,25617901
nested_graph_evaluation: 32883284,33041973,32939767,32804371,33086414,32841583,32839392,32932466,32767988
```

### Baseline run 2

```text
flat_graph_evaluation: 21207029,20979096,21086013,20993527,21046720,21001798,21136744,21055221,21090321
scalar_output_evaluation: 27671980,27750592,27725513,27789485,27671990,27727472,27770353,27839899,27734831
nested_graph_evaluation: 30874000,30996634,31022185,31008856,31007604,30906640,32651122,31054018,31116610
```

### Baseline run 3

```text
flat_graph_evaluation: 20847720,21106792,21142854,20880050,20942125,21787158,21571197,21628000,21256390
scalar_output_evaluation: 28463500,28038617,28114932,28498482,29493644,28672901,27932432,28680672,28602217
nested_graph_evaluation: 32007697,31246396,31871359,31248257,31168312,31659059,31390825,31394204,51676083
```

### Candidate run 3

```text
flat_graph_evaluation: 22016980,22057642,22249024,22091364,22084813,22105655,22131237,22101274,22216380
scalar_output_evaluation: 25944877,26034043,25883604,26042522,26118176,25932996,26015880,25952517,26341289
nested_graph_evaluation: 34388532,34391753,34291758,34475436,34413544,34332839,34328188,34466767,34589823
```

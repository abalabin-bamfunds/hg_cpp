## Expression

| Operation | Layout | Before ms | After ms | Speedup |
| --- | --- | ---: | ---: | ---: |
| payload-only update | clustered holes | 0.076 | 0.075 | 1.00× |
| payload-only update | scattered holes | 0.078 | 0.075 | 1.03× |
| predicate update preserving membership | clustered holes | 0.143 | 0.136 | 1.05× |
| predicate update preserving membership | scattered holes | 0.154 | 0.149 | 1.03× |
| predicate update changing membership | clustered holes | 0.969 | 0.480 | 2.02× |
| predicate update changing membership | scattered holes | 0.996 | 0.596 | 1.67× |
| threshold update preserving membership | clustered holes | 5.330 | 0.972 | 5.48× |
| threshold update preserving membership | scattered holes | 6.367 | 2.143 | 2.97× |
| threshold update changing membership | clustered holes | 5.637 | 1.787 | 3.15× |
| threshold update changing membership | scattered holes | 6.743 | 3.138 | 2.15× |
| row add/remove | clustered holes | 1.326 | 0.846 | 1.57× |
| row add/remove | scattered holes | 1.486 | 1.082 | 1.37× |

## Arrow

| Selection and deletion | Method | Chunks before → after | Consumer before ms | Consumer after ms | Speedup | Graph before ms | Graph after ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 50% clustered selection, clustered deletion | native batch sum | 14 → 15 | 0.413 | 0.098 | 4.23× | 1.332 | 0.482 |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 14 → 15 | 0.518 | 0.172 | 3.01× | 1.412 | 0.560 |
| 50% clustered selection, clustered deletion | dense gather + sum | 1 → 1 | 0.648 | 0.358 | 1.81× | 1.539 | 0.746 |
| 50% scattered selection, clustered deletion | native batch sum | 49168 → 49168 | 0.970 | 0.674 | 1.44× | 1.933 | 1.054 |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 49168 → 49168 | 16.546 | 15.747 | 1.05× | 17.494 | 16.191 |
| 50% scattered selection, clustered deletion | dense gather + sum | 1 → 1 | 1.216 | 1.058 | 1.15× | 2.113 | 1.438 |
| 50% clustered selection, scattered deletion | native batch sum | 638 → 638 | 0.440 | 0.167 | 2.63× | 1.346 | 0.577 |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 638 → 638 | 0.632 | 0.341 | 1.85× | 1.571 | 0.757 |
| 50% clustered selection, scattered deletion | dense gather + sum | 1 → 1 | 0.562 | 0.330 | 1.70× | 1.480 | 0.742 |
| 50% scattered selection, scattered deletion | native batch sum | 49168 → 49168 | 0.971 | 0.677 | 1.44× | 1.885 | 1.073 |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 49168 → 49168 | 16.821 | 16.084 | 1.05× | 17.814 | 16.596 |
| 50% scattered selection, scattered deletion | dense gather + sum | 1 → 1 | 1.223 | 1.061 | 1.15× | 2.138 | 1.462 |
| 100% selection, no deletion | native batch sum | 15 → 15 | 0.796 | 0.085 | 9.37× | 0.811 | 0.099 |
| 100% selection, no deletion | borrowed Arrow + sum | 15 → 15 | 0.992 | 0.219 | 4.52× | 1.011 | 0.234 |
| 100% selection, no deletion | dense gather + sum | 1 → 1 | 1.036 | 0.664 | 1.56× | 1.058 | 0.682 |
| 10% scattered selection and deletion | native batch sum | 9001 → 9001 | 0.187 | 0.131 | 1.43× | 0.892 | 0.482 |
| 10% scattered selection and deletion | borrowed Arrow + sum | 9001 → 9001 | 2.974 | 2.828 | 1.05× | 3.670 | 3.180 |
| 10% scattered selection and deletion | dense gather + sum | 1 → 1 | 0.234 | 0.208 | 1.13× | 0.946 | 0.552 |

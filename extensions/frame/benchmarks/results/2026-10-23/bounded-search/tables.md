# Bounded validity search comparison

| Selection and deletion | Method | Before ms | After ms | Speedup |
| --- | --- | ---: | ---: | ---: |
| 50% clustered selection, clustered deletion | native batch sum | 0.099 | 0.098 | 1.01× |
| 50% clustered selection, clustered deletion | borrowed Arrow + sum | 0.180 | 0.081 | 2.22× |
| 50% clustered selection, clustered deletion | dense gather + sum | 0.367 | 0.366 | 1.00× |
| 50% scattered selection, clustered deletion | native batch sum | 0.673 | 0.673 | 1.00× |
| 50% scattered selection, clustered deletion | borrowed Arrow + sum | 16.530 | 16.444 | 1.01× |
| 50% scattered selection, clustered deletion | dense gather + sum | 1.057 | 1.087 | 0.97× |
| 50% clustered selection, scattered deletion | native batch sum | 0.166 | 0.166 | 1.00× |
| 50% clustered selection, scattered deletion | borrowed Arrow + sum | 0.329 | 0.255 | 1.29× |
| 50% clustered selection, scattered deletion | dense gather + sum | 0.332 | 0.339 | 0.98× |
| 50% scattered selection, scattered deletion | native batch sum | 0.675 | 0.675 | 1.00× |
| 50% scattered selection, scattered deletion | borrowed Arrow + sum | 16.167 | 16.233 | 1.00× |
| 50% scattered selection, scattered deletion | dense gather + sum | 1.071 | 1.076 | 0.99× |
| 100% selection, no deletion | native batch sum | 0.085 | 0.086 | 0.99× |
| 100% selection, no deletion | borrowed Arrow + sum | 0.220 | 0.032 | 6.84× |
| 100% selection, no deletion | dense gather + sum | 0.396 | 0.411 | 0.96× |
| 10% scattered selection and deletion | native batch sum | 0.130 | 0.130 | 1.00× |
| 10% scattered selection and deletion | borrowed Arrow + sum | 2.820 | 2.869 | 0.98× |
| 10% scattered selection and deletion | dense gather + sum | 0.206 | 0.210 | 0.98× |

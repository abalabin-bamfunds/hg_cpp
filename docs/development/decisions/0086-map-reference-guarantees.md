# Map reference guarantees

Map item references are constant only when their source controls branch
lifetime. A selected key set controls lifetime; without one, only a sole mapped
TSD does. This keeps other mapped inputs mutable when their items can change
while the branch remains alive. Explicit item-reference and root-dictionary
reference guarantees limit the generated item binding. See [map](../design/map.md#arguments).

A non-reference inner graph output has one fixed output handle per branch, so
the map output exposes a constant reference to that handle. An explicit
reference output retains its declared reference guarantee. See
[map outputs](../design/map.md#output).

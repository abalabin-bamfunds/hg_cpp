# bg

`bg` is a C++23 engine for forward propagation through computational graphs. See
the [project definition](docs/project.md) for its scope and priorities. The
[terminology dictionary](docs/terminology.md) defines project-specific terms and
core entities.

Current design documents cover the [node API](docs/node-api.md),
[node object layout](docs/node-layout.md),
[graph wiring](docs/graph-wiring.md),
[operators](docs/operators.md),
[evaluation](docs/evaluation.md),
[type system](docs/type-system.md),
[scalar API](docs/scalar-api.md),
[global state](docs/global-state.md),
[nested graphs](docs/nested-graphs.md),
[map](docs/map.md),
[reduce](docs/reduce.md),
[mesh](docs/mesh.md),
[switch](docs/switch.md),
[testing](docs/testing.md),
[time series types](docs/time-series-types.md),
[time series references](docs/time-series-references.md),
[time series validity](docs/time-series-validity.md), and
[TSD layout](docs/tsd-layout.md).

## Build and test

Builds use the `hg-build` Conda environment. Activate it before using the
development preset:

```sh
conda activate hg-build
cmake --preset dev
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure
```

The project requires CMake 3.31 or newer, Ninja, Catch2 3, and a C++23 compiler.
CMake downloads pinned `ddvisitor` and `tpack` releases during configuration.

After installation, CMake consumers can use
`find_package(bg CONFIG REQUIRED)` and link to `bg::bg`.

## Python bindings

The optional Python package uses nanobind. Install nanobind and pytest in the
Python environment selected by CMake, then configure and build with:

```sh
python -m pip install nanobind pytest
cmake --preset dev -DBG_BUILD_PYTHON=ON -DBUILD_TESTING=ON
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure -R '^python_tests$'
```

This builds the `bg::python` CMake target and the Python package in
`build/dev/bindings/python`. Installed CMake consumers can link `bg::python`;
request it with `find_package(bg COMPONENTS python REQUIRED)`. Consumers that
use only `bg::bg` do not require Python or nanobind. See [Python
integration](docs/python.md) for the implemented API and package details. The
`test` optional dependency in `pyproject.toml` installs pytest. To build a
wheel, install `scikit-build-core` and run `python -m pip wheel .`.

## Layout

- `include/bg/` — public C++ headers
- `src/` — C++ implementation
- `tests/` — Catch2 unit tests
- `bindings/python/` — optional Python extension
- `cmake/` — CMake package support

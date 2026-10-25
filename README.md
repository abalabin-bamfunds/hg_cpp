# bg

`bg` is a C++23 engine for forward propagation through computational graphs.

## User guide

Start with the [BG documentation](docs/user/index.md). The opening chapters use
plain value types and introduce graphs through matching Python and C++
examples.

Build and preview the documentation site with:

```sh
python -m pip install -r docs/requirements.txt
python -m sphinx -b html --fail-on-warning --keep-going docs/user build/docs/html
python -m http.server 8000 --directory build/docs/html
```

Open <http://localhost:8000>. See the [documentation map](docs/README.md) for
build commands and authoring guidance.

## Development documentation

The [development documentation](docs/development/index.md) covers engine design,
implementation guidance, testing, decision records, and proposals. The
[project definition](docs/development/project.md) sets scope and priorities. Both
documentation trees follow the shared [terminology dictionary](docs/terminology.md).

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
integration](docs/development/design/python.md) for the implemented API and package details. The
`test` optional dependency in `pyproject.toml` installs pytest. To build a
wheel, install `scikit-build-core` and run `python -m pip wheel .`.

## Layout

- `include/bg/` — public C++ headers
- `src/` — C++ implementation
- `tests/` — Catch2 unit tests
- `bindings/python/` — optional Python extension
- `cmake/` — CMake package support

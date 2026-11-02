# hg

`hg` is a C++23 engine for forward propagation through computational graphs.

## User guide

Start with the [HG documentation](docs/user/index.md). The opening chapters use
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

The project requires CMake 3.31 or newer, Ninja, Catch2 3, fmt, ICU, and a C++23 compiler.
GCC builds require GCC 14 or newer.
CMake downloads pinned `ddvisitor`, `tpack`, and Quill releases during configuration.

On macOS, when using Homebrew dependencies, provide their prefixes to CMake:

```sh
cmake --preset dev -DCMAKE_PREFIX_PATH="$(brew --prefix icu4c);$(brew --prefix fmt)"
```

After installation, CMake consumers can use
`find_package(hg CONFIG REQUIRED)` and link to `hg::hg`.

## Python bindings

The `hg-cpp` distribution provides the `hgraph` Python module through nanobind.
Install nanobind and pytest in the Python environment selected by CMake, then
configure and build with:

```sh
python -m pip install nanobind pytest
cmake --preset dev -DHG_BUILD_PYTHON=ON -DBUILD_TESTING=ON
cmake --build --preset dev
ctest --test-dir build/dev --output-on-failure -R '^python_tests$'
```

This builds the `hg::python` CMake target and the Python package in
`build/dev/bindings/python/hgraph`. Installed CMake consumers can link `hg::python`;
request it with `find_package(hg COMPONENTS python REQUIRED)`. Consumers that
use only `hg::hg` do not require Python or nanobind. See [Python
integration](docs/development/design/python.md) for the implemented API and package details. The
`test` optional dependency in `pyproject.toml` installs pytest. To build a
wheel, install `scikit-build-core` and run `python -m pip wheel .`.

## Layout

- `include/hg/` — public C++ headers
- `src/` — C++ implementation
- `tests/` — Catch2 unit tests
- `bindings/python/` — optional Python extension
- `cmake/` — CMake package support

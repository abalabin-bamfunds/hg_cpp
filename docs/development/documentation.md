# Documentation maintenance

The published documentation uses Sphinx, MyST Markdown, Sphinx Design, and Furo.
The [documentation map](../README.md) provides build and preview commands.
The build needs Python 3.13 or later. It does not import BG or compile the engine.
`.readthedocs.yaml` uses `docs/user/conf.py` and `docs/requirements.txt`.
Publication requires importing this repository into Read the Docs.

## Structure

The [documentation plan](planning/user-documentation-plan.md) defines chapter boundaries.
`docs/user/index.md` owns navigation. Sphinx reads only `docs/user/`; development
documents and the shared [terminology dictionary](../terminology.md) remain outside
the site. Paths in the following table are relative to `docs/user/`.

| Directory | Content |
| --- | --- |
| `guide/getting-started.md` | Basic syntax crash course |
| `guide/graphs/` | Graph patterns |
| `guide/nodes/` | Plain value node writing |
| `guide/advanced-syntax.md` | Advanced syntax overview |
| `guide/advanced-graphs/` | Time series, collections, references, guarantees, sharing |
| `guide/advanced-nodes/` | Input views, publication, state, scheduling |
| `guide/execution/` | Applications and time patterns |
| `how-to/` | Testing, diagnostics, integration, worked applications |
| `reference/` | API contracts and library operations |

Use relative Markdown links and descriptive anchors. Keep one primary explanation
for each concept. Introduce its purpose, show code and results, then state constraints.
Quick starts link to detailed chapters. Avoid lesson narration, exercises, and
instructions to modify an earlier program.

## Examples

Use complete Python and C++ examples with equivalent inputs and expected behavior.
Start with plain types. Add explicit time series declarations when their contracts
are needed. Python imports names directly. C++ uses `using namespace bg;` and the
required `bg::operators`, `literal`, or `bg::literals` imports. Use literal input
sequences for `run_graph`.

Language tabs use `:sync-group: language` with `:sync: python` and `:sync: cpp`.
Each C++ tab contains nested **Code** and **Build and run** tabs. The first shows
source and expected output; the second links to the example project and gives build
commands. Existing chapters provide the full directive structure.

Mark pages containing complete examples with `<!-- runnable-example -->`.
Place `Output:` and a fenced `text` block immediately after each runnable source
block. Signature fragments omit an output block and are not executed. Use
`Output (ellipsis):` only for variable fields such as timing or generated paths.
Plain output labels require exact matches.

`docs/tools/example_sources.py` collects examples from the homepage, guide, task pages,
and reference. The checker and Sphinx download generator use the same sources.
Generated sources belong to ignored `docs/user/_generated/` and `build/doc-examples/`.

## Verification

Run the tooling tests and strict site build after changing examples or navigation:

```sh
python -m unittest discover -s docs/tools/tests -v
python -m sphinx -E -a -b html --fail-on-warning --keep-going docs/user build/docs/html
```

Use a Python environment containing a BG build from the same revision to execute
examples. Supply its native CMake installation to compile the C++ examples:

```sh
python docs/tools/check_examples.py --cmake-prefix /absolute/path/to/install --jobs 2
```

`--cpp-compiler` selects a C++23 compiler and `--cmake` selects CMake. Without
`--cmake-prefix`, the checker executes Python and extracts C++ sources. An existing
CMake cache must resolve `bg_DIR` to the requested installation.

Review the rendered desktop and mobile layouts, both languages, nested build tabs,
search, diagrams, and downloadable source projects. The native extension example has
a separate CMake project and must also be built when its source or setup changes.

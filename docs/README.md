# Build the documentation

The user documentation uses Sphinx, MyST Markdown, and the Furo theme. Build it
with Python 3.13 or later. The documentation build does not import BG or compile
the C++ engine.

Run these commands from the repository directory, with a Python virtual
environment active:

```sh
python -m pip install -r docs/requirements.txt
python -m sphinx -a -b html --fail-on-warning --keep-going docs build/docs/html
python -m http.server 8000 --directory build/docs/html
```

Open <http://localhost:8000>. Search, chapter navigation, page contents, and
light/dark appearance are provided by the theme.

## Add pages

Put guide chapters in `docs/guide/`, task guides in `docs/how-to/`, and reference
pages in `docs/reference/`.
Add each published page to a `toctree` in `docs/index.md` or its section index.
Use relative Markdown links between pages and fenced code blocks with language
names for examples.

Group chapters by the sections in the [documentation plan](user-documentation-plan.md#documentation-structure).
The sidebar includes the complete guide, task guides, API reference, and library
reference. Keep its captions consistent with the plan and introduction.
Changing a navigation group does not require moving its chapter files.

Provide Python and C++ versions of each user-facing example, including setup,
run commands, expected results, and language-specific explanations. Keep inputs
and expected updates equivalent. Use plain value types until the example needs
explicit time series annotations in node and graph definitions. In Python
examples, import the names used directly, such as `from bg import graph, test_graph`,
and use unqualified decorators and calls. In C++ examples, use
`using namespace bg;` and `using namespace bg::testing;` to keep calls short.
Import `using namespace literal;` when an example uses literal sentinels, and
`using namespace bg::literals;` for `_n` named arguments. Use
literal input sequences for `test_graph` and `SKIP` for skipped updates. Infer
input types from plain parameters; supply an explicit type with `input<TS<...>>`
only when the signature leaves it unresolved. Use literal sequences for expected
results too.
Verify both versions against the current API.

The opening sequence is a fixed-value program, arithmetic, graph functions,
then changing values. Keep the first `test_graph` calls free of input arguments.
Introduce update sequences, their output encoding, and fixed versus changing
arguments in Values that change. Each concept has one primary explanation;
later chapters use it and link back when needed. The Graph functions chapter
covers parameters and calls; Values that change covers input update behavior.
Use `const_` when the fixed-value examples need a graph value; expressions using
only language literals run during construction.

Lead each chapter or section with a concrete need and the API that addresses it:
“To use a changing offset, pass it as a second input.” Explain what the example
does and when it is useful. Preserve a gradual introduction of syntax without
framing chapters as lessons or exercises. Avoid prediction prompts, mandatory
code changes, and file-replacement instructions in the explanations. Keep
operational steps for installation, building, and running examples. Label excerpts
with their required definitions and imports; provide a complete example for each
new task when practical.

Use synchronized language tabs with this MyST structure:

`````md
::::{tab-set}
:sync-group: language

:::{tab-item} Python
:sync: python

```python
# Python example
```
:::

:::{tab-item} C++
:sync: cpp

```cpp
// C++ example
```
:::
::::
`````

Put language-specific instructions inside the tabs. Keep shared concepts and
update tables outside them. The `language` synchronization group remembers
the selected language across examples and pages.

Inside the C++ tab, use nested **Code** and **Build and run** tabs when an
example needs build instructions. Put the example, expected output, and
explanation in **Code**, selected by default. Put file setup, CMake configuration,
and build and run commands in **Build and run**. Keep these inner tabs
unsynchronized and use longer directive fences for the enclosing language tabs.

`docs/conf.py` selects the user documentation as the site source. Engine design
documents, decision records, this file, and the user documentation plan remain
outside the published site.

## Verify examples

Use a Python environment containing a BG build from the same revision. After
installing the native CMake package, run:

```sh
python docs/check_examples.py --cmake-prefix install --jobs 2
```

This runs Python examples, compiles C++ examples, and compares their output with
the documentation. `--cpp-compiler /path/to/c++` selects a C++23 compiler;
`--cmake /path/to/cmake` selects CMake. Without `--cmake-prefix`, the checker runs
Python and extracts C++ sources without compiling them. Generated files belong
to `build/doc-examples/`. The native extension task has a separate CMake project
shown on its page and must also be built and run when changed.

Mark new pages containing complete examples with `<!-- runnable-example -->`.
For each pair, place `Output:` and a fenced `text` block after each Python/C++
code block. The checker executes every pair on a marked page. The opening
chapters also contain fragments; the checker supplies their documented context.

The build treats warnings as errors and checks unresolved references. Keep
links valid and include each published page in the navigation. Mermaid code
blocks render as diagrams in HTML.

## Read the Docs

The root `.readthedocs.yaml` configures the build OS, Python version,
documentation dependencies, Sphinx configuration, and failure on warnings.
Read the Docs uses the same source and configuration as the local build.

To publish, import the repository into a Read the Docs project and select the
branch to build. The repository must be reachable by that project. Hosting
activation and its public URL are configured in Read the Docs.

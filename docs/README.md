# Build the documentation

The user documentation uses Sphinx, MyST Markdown, and the Furo theme. Build it
with Python 3.13 or later. The documentation build does not import BG or compile
the C++ engine.

Run these commands from the repository directory, with a Python virtual
environment active:

```sh
python -m pip install -r docs/requirements.txt
python -m sphinx -b html --fail-on-warning --keep-going docs build/docs/html
python -m http.server 8000 --directory build/docs/html
```

Open <http://localhost:8000>. Search, chapter navigation, page contents, and
light/dark appearance are provided by the theme.

## Add pages

Put learning chapters in `docs/guide/` and reference pages in `docs/reference/`.
Add each published page to a `toctree` in `docs/index.md` or its section index.
Use relative Markdown links between pages and fenced code blocks with language
names for examples.

Provide Python and C++ versions of each user-facing example, including setup,
run commands, expected results, and language-specific explanations. Keep inputs
and expected updates equivalent. Use plain value types until the lesson needs
explicit time series annotations in node and graph definitions. Use literal
input sequences for C++ `run_graph` examples and `literal::SKIP` for skipped
updates. Infer input types from plain parameters; supply an explicit type with
`input<TS<...>>` only when the signature leaves it unresolved. Use
literal sequences for expected results too.
Verify both versions against the current API.

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

`docs/conf.py` selects the user documentation as the site source. Engine design
documents, decision records, this file, and the user documentation plan remain
outside the published site.

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

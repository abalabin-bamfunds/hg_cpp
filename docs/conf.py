from pathlib import Path
import sys
import tomllib

DOCS = Path(__file__).resolve().parent
sys.path.insert(0, str(DOCS))

from example_sources import collect_examples, write_downloads


def generate_example_downloads(app):
    """Build downloads from the same collected sources as the example checker."""
    write_downloads(collect_examples(DOCS.parent), DOCS)


def setup(app):
    app.connect("builder-inited", generate_example_downloads)
    return {"parallel_read_safe": True}


project = "BG"
release = tomllib.loads((Path(__file__).resolve().parents[1] / "pyproject.toml").read_text())["project"]["version"]

extensions = ["myst_parser", "sphinxcontrib.mermaid", "sphinx_design"]
source_suffix = {".md": "markdown"}
root_doc = "index"

# Publish the user documentation; engine documents remain in the repository.
include_patterns = ["index.md", "guide/**", "how-to/**", "reference/**"]
exclude_patterns = ["_generated/**"]
nitpicky = True
myst_heading_anchors = 3
myst_enable_extensions = ["colon_fence"]
myst_fence_as_directive = ["mermaid"]
mermaid_height = "auto"

html_theme = "furo"
html_title = "BG documentation"
html_show_copyright = False
html_theme_options = {"navigation_with_keys": True}

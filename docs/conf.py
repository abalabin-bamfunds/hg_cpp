from pathlib import Path
import tomllib


project = "BG"
release = tomllib.loads((Path(__file__).resolve().parents[1] / "pyproject.toml").read_text())["project"]["version"]

extensions = ["myst_parser", "sphinxcontrib.mermaid", "sphinx_design"]
source_suffix = {".md": "markdown"}
root_doc = "index"

# Publish the user documentation; engine documents remain in the repository.
include_patterns = ["index.md", "guide/**", "how-to/**", "reference/**"]
nitpicky = True
myst_heading_anchors = 3
myst_enable_extensions = ["colon_fence"]
myst_fence_as_directive = ["mermaid"]
mermaid_height = "auto"

html_theme = "furo"
html_title = "BG documentation"
html_show_copyright = False
html_theme_options = {"navigation_with_keys": True}

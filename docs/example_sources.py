"""Collect, verify, and package the runnable examples published in the docs.

This module uses only the Python standard library so Sphinx can create example
downloads without importing BG or invoking a compiler.
"""

from dataclasses import dataclass
from doctest import ELLIPSIS, OutputChecker
from pathlib import Path
import re
import shutil
from zipfile import ZIP_DEFLATED, ZipFile


@dataclass(frozen=True)
class Fence:
    language: str
    code: str
    start: int
    end: int


@dataclass(frozen=True)
class Example:
    name: str
    slug: str
    page: str
    title: str
    python: str
    python_output: str
    python_ellipsis: bool
    cpp: str
    cpp_output: str
    cpp_ellipsis: bool


def fences(source: str) -> list[Fence]:
    """Return Markdown fenced blocks with their source offsets."""
    lines = source.splitlines(keepends=True)
    result = []
    offset = 0
    index = 0

    while index < len(lines):
        line = lines[index]
        opening = re.match(r" {0,3}(?P<fence>`{3,}|~{3,})(?P<info>[^\r\n]*)\r?\n?$", line)
        if opening is None:
            offset += len(line)
            index += 1
            continue

        marker = opening.group("fence")
        info = opening.group("info").strip()
        language = info.split(maxsplit=1)[0] if info else ""
        start = offset
        offset += len(line)
        index += 1
        body_lines = []

        while index < len(lines):
            line = lines[index]
            closing = re.match(
                r" {0,3}(?P<fence>`{" + str(len(marker)) + r",}|~{" + str(len(marker))
                + r",})[ \t]*\r?\n?$",
                line,
            )
            if closing is not None and closing.group("fence")[0] == marker[0]:
                end = offset + len(line)
                code = "".join(body_lines).removesuffix("\n").removesuffix("\r")
                result.append(Fence(language, code, start, end))
                offset = end
                index += 1
                break

            body_lines.append(line)
            offset += len(line)
            index += 1
        else:
            raise ValueError(f"Unclosed Markdown fence beginning at character offset {start}")

    return result


def expected_output(source: str, code_fence: Fence, all_fences: list[Fence]) -> tuple[str, bool] | None:
    """Read an adjacent output block; ellipsis matching requires an explicit label."""
    following = next((fence for fence in all_fences if fence.start >= code_fence.end), None)
    if following is None or following.language != "text":
        return None

    between = source[code_fence.end:following.start]
    label = re.fullmatch(r"\s*Output(?P<ellipsis> \(ellipsis\))?:\s*", between)
    if label is None:
        return None
    return following.code, label.group("ellipsis") is not None


def output_matches(expected: str, actual: str, ellipsis: bool = False) -> bool:
    """Compare output exactly unless its expected block opted into ellipsis matching."""
    flags = ELLIPSIS if ellipsis else 0
    return OutputChecker().check_output(expected.strip(), actual.strip(), flags)


def split_cpp_outputs(stdout: str, start: int, count: int) -> list[str]:
    """Split a compiled batch at its per-example output markers."""
    outputs = []
    remaining = stdout
    for index in range(start, start + count):
        marker = f"BG_DOC_EXAMPLE_{index}"
        if marker not in remaining:
            raise ValueError(f"output marker {marker} is missing")
        output, remaining = remaining.split(marker, maxsplit=1)
        outputs.append(output)
    return outputs


def _title(source: str, fallback: str) -> str:
    heading = re.search(r"^#\s+(.+?)\s*#*\s*$", source, re.M)
    return heading.group(1) if heading else fallback


def _slug(page: str, number: int) -> str:
    stem = Path(page).with_suffix("").as_posix()
    return f"{stem.replace('/', '-')}-{number:02d}"


def collect_examples(root: Path) -> list[Example]:
    """Collect the opening fragments and complete paired examples."""
    docs = root / "docs"
    examples = []
    page_counts: dict[str, int] = {}

    def add(page: str, python: str, python_output: str, cpp: str, cpp_output: str = "true",
            python_ellipsis: bool = False, cpp_ellipsis: bool = False) -> None:
        relative_page = f"guide/{page}" if not page.startswith(("guide/", "how-to/", "reference/")) else page
        source = (docs / relative_page).read_text()
        number = page_counts.get(relative_page, 0) + 1
        page_counts[relative_page] = number
        examples.append(Example(
            f"{relative_page}:{number}", _slug(relative_page, number), relative_page,
            _title(source, Path(relative_page).stem.replace("-", " ").title()),
            python.strip(), python_output, python_ellipsis, cpp.strip(), cpp_output, cpp_ellipsis,
        ))

    def page_fences(page: str) -> tuple[str, list[Fence]]:
        source = (docs / "guide" / page).read_text()
        return source, fences(source)

    for page, output in (
        ("getting-started.md", "[18]"),
        ("arithmetic.md", "[200]"),
        ("connect-calculations.md", "[200]"),
    ):
        source, blocks = page_fences(page)
        python = next(block.code for block in blocks if block.language == "python")
        cpp = next(block.code for block in blocks if block.language == "cpp")
        add(page, python, output, cpp)

    _, blocks = page_fences("changing-values.md")
    python_blocks = [block.code for block in blocks if block.language == "python"]
    cpp_blocks = [block.code for block in blocks if block.language == "cpp"]
    outputs = ("[200, 220, 210]", "[200, None, 200, 210]", "[None, 200, 210, 220]", "[200, 220]")
    for index, output in enumerate(outputs):
        python = python_blocks[index]
        cpp = cpp_blocks[index]
        if index in (1, 3):
            base = python_blocks[0 if index == 1 else 2]
            python = base[:base.index("print(test_graph(")] + python
            base = cpp_blocks[0 if index == 1 else 2]
            cpp = base[:base.index("int main()")] + "int main() {\n" + cpp + "\n}\n"
        add("changing-values.md", python, output, cpp)

    _, blocks = page_fences("conditions.md")
    python_blocks = [block.code for block in blocks if block.language == "python"]
    cpp_blocks = [block.code for block in blocks if block.language == "cpp"]
    for index, output in ((0, "[20, 21, 21]"), (2, "[20, 22, 21]"),
                          (3, "[False, True, True, False]"), (4, "[-1, 0, 1, -1]")):
        add("conditions.md", python_blocks[index], output, cpp_blocks[index])

    _, blocks = page_fences("dedup.md")
    python_blocks = [block.code for block in blocks if block.language == "python"]
    cpp_blocks = [block.code for block in blocks if block.language == "cpp"]
    for index in range(2):
        python = python_blocks[index]
        cpp = cpp_blocks[index]
        if index == 1:
            python = (python_blocks[0][:python_blocks[0].index("@graph")] + python + "\n\n"
                      + python_blocks[0][python_blocks[0].index("print(test_graph("):])
            cpp = (cpp_blocks[0][:cpp_blocks[0].index("auto reported_temperature")] + cpp + "\n\n"
                   + cpp_blocks[0][cpp_blocks[0].index("int main()") :])
        add("dedup.md", python, "[20, None, None, 21, 20, None]", cpp)

    for page, outputs in (
        ("filter.md", ["[None, 20, None, None, 22, None, None, 22]"]),
        ("sample.md", ["[None, 21, 21, None, 23, 23]"]),
        ("count-and-lag.md", ["[None, 1, 2, None, 3, 4]", "[None, None, 20, None, 20, 26]"]),
        ("running-calculations.md", ["[None, 20, 40, None, 66, 90]",
                                      "[None, 20.0, 20.0, None, 22.0, 22.5]"]),
    ):
        _, blocks = page_fences(page)
        python_blocks = [block.code for block in blocks if block.language == "python"]
        cpp_blocks = [block.code for block in blocks if block.language == "cpp"]
        for index, output in enumerate(outputs):
            add(page, python_blocks[index], output, cpp_blocks[index])

    for path in sorted(docs.rglob("*.md")):
        if path.name.startswith("_") or path.parent.name == "_generated":
            continue
        source = path.read_text()
        if "<!-- runnable-example -->" not in source.splitlines():
            continue

        blocks = fences(source)
        paired = {language: [] for language in ("python", "cpp")}
        for block in blocks:
            if block.language not in paired:
                continue
            output = expected_output(source, block, blocks)
            if output is not None:
                paired[block.language].append((block.code, *output))

        if len(paired["python"]) != len(paired["cpp"]) or not paired["python"]:
            raise ValueError(
                f"{path.relative_to(root)}: expected the same nonzero number of paired Python and C++ examples; "
                f"found {len(paired['python'])} and {len(paired['cpp'])}"
            )

        relative_page = path.relative_to(docs).as_posix()
        for python, cpp in zip(paired["python"], paired["cpp"]):
            add(relative_page, python[0], python[1], cpp[0], cpp[1], python[2], cpp[2])

    return examples


def write_downloads(examples: list[Example], docs: Path) -> None:
    """Write Python files, C++ project archives, and their Markdown catalog."""
    generated = docs / "_generated"
    publish = generated / "publish"
    downloads = publish / "downloads"
    if publish.exists():
        shutil.rmtree(publish)
    downloads.mkdir(parents=True, exist_ok=True)

    for example in examples:
        (downloads / f"{example.slug}.py").write_text(example.python.rstrip() + "\n")
        with ZipFile(downloads / f"{example.slug}.zip", "w", ZIP_DEFLATED) as archive:
            archive.writestr("main.cpp", example.cpp.rstrip() + "\n")
            archive.writestr(
                "CMakeLists.txt",
                "\n".join((
                    "cmake_minimum_required(VERSION 3.31)",
                    f"project({example.slug.replace('-', '_')} LANGUAGES CXX)",
                    "find_package(bg CONFIG REQUIRED)",
                    "add_executable(example main.cpp)",
                    "target_link_libraries(example PRIVATE bg::library)",
                    "",
                )),
            )
            archive.writestr(
                "README.md",
                "\n".join((
                    f"# {example.title}",
                    "",
                    "## Build and run",
                    "",
                    "This project requires an installed BG CMake package and CMake 3.31 or newer.",
                    "",
                    "```sh",
                    "cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install",
                    "cmake --build build --config Release",
                    "```",
                    "",
                    "Run `./build/example` on Linux or macOS. With a multi-configuration",
                    "generator, run `build/Release/example.exe` on Windows.",
                    "",
                )),
            )

    rows = ["| Example | Python (.py) | C++ project (.zip) |", "| --- | --- | --- |"]
    page_counts = {}
    for example in examples:
        page_counts[example.page] = page_counts.get(example.page, 0) + 1

    page_examples = {}
    for example in examples:
        page_examples[example.page] = page_examples.get(example.page, 0) + 1
        label = example.title
        if page_counts[example.page] > 1:
            label = f"{label}, example {page_examples[example.page]}"
        label = label.replace("|", "\\|")
        rows.append(
            f"| [{label}](../{example.page}) "
            f"| [Download](../downloads/{example.slug}.py) "
            f"| [Project](../downloads/{example.slug}.zip) |"
        )
    (generated / "examples.md").write_text("\n".join(rows) + "\n")

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
    """Collect complete Python and C++ examples marked in published user documentation."""
    docs = root / "docs/user"
    examples = []

    pages = [docs / "index.md"]
    for section in ("guide", "how-to", "reference"):
        pages.extend(sorted((docs / section).rglob("*.md")))

    for path in pages:
        if not path.exists():
            continue

        relative_page = path.relative_to(docs).as_posix()
        if "_generated" in Path(relative_page).parts:
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
            if output is None:
                continue
            paired[block.language].append((block.code, *output))

        if len(paired["python"]) != len(paired["cpp"]) or not paired["python"]:
            raise ValueError(
                f"{relative_page}: expected the same nonzero number of paired Python and C++ examples; "
                f"found {len(paired['python'])} and {len(paired['cpp'])}"
            )

        page = relative_page
        title = _title(source, path.stem.replace("-", " ").title())
        for number, (python, cpp) in enumerate(zip(paired["python"], paired["cpp"]), start=1):
            examples.append(Example(
                name=f"{page}:{number}",
                slug=_slug(page, number),
                page=page,
                title=title,
                python=python[0].strip(),
                python_output=python[1],
                python_ellipsis=python[2],
                cpp=cpp[0].strip(),
                cpp_output=cpp[1],
                cpp_ellipsis=cpp[2],
            ))

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
            f"| {{download}}`Download <../_generated/publish/downloads/{example.slug}.py>` "
            f"| {{download}}`Project <../_generated/publish/downloads/{example.slug}.zip>` |"
        )
    (generated / "examples.md").write_text("\n".join(rows) + "\n")

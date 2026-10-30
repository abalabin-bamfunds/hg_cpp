"""Collect, verify, and package the runnable examples published in the docs.

This module uses only the Python standard library so Sphinx can create example
downloads without importing BG or invoking a compiler.
"""

from dataclasses import dataclass
from doctest import ELLIPSIS, OutputChecker
from pathlib import Path, PurePosixPath
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
    cpp_libraries: tuple[str, ...] = ("bg::library",)
    cmake_source: str = ""


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


def _metadata(source: str, name: str) -> tuple[str, ...]:
    matches = re.findall(r"^<!-- " + re.escape(name) + r":\s*(.*?)\s*-->$", source, re.M)
    if len(matches) > 1:
        raise ValueError(f"example metadata {name!r} must appear at most once per page")
    return tuple(matches[0].split()) if matches else ()


def _source_metadata(source: str) -> str:
    matches = re.findall(r"^<!-- example-source:\s*(.*?)\s*-->$", source, re.M)
    if len(matches) > 1:
        raise ValueError("example metadata 'example-source' must appear at most once per page")
    if not matches:
        return ""

    source_value = matches[0].strip()
    source_path = PurePosixPath(source_value)
    if not source_value or "\\" in source_value or source_path.is_absolute() or ".." in source_path.parts:
        raise ValueError("example-source must be a nonempty path inside BG_SOURCE_DIR")
    return source_path.as_posix()


def collect_examples(root: Path) -> list[Example]:
    """Collect complete Python and C++ examples marked in published user documentation."""
    docs = root / "docs/user"
    examples = []

    pages = [docs / "index.md"]
    for section in ("guide", "how-to", "reference", "extensions"):
        pages.extend(sorted((docs / section).rglob("*.md")))

    for path in pages:
        if not path.exists():
            continue

        relative_page = path.relative_to(docs).as_posix()
        if "_generated" in Path(relative_page).parts:
            continue

        source = path.read_text()
        lines = source.splitlines()
        paired_marker = "<!-- runnable-example -->" in lines
        cpp_only_marker = "<!-- runnable-example: cpp -->" in lines
        if not paired_marker and not cpp_only_marker:
            continue
        if paired_marker and cpp_only_marker:
            raise ValueError(f"{relative_page}: use only one runnable-example marker")

        blocks = fences(source)
        paired = {language: [] for language in ("python", "cpp")}
        for block in blocks:
            if block.language not in paired:
                continue
            output = expected_output(source, block, blocks)
            if output is None:
                continue
            paired[block.language].append((block.code, *output))

        if cpp_only_marker:
            if paired["python"] or not paired["cpp"]:
                raise ValueError(
                    f"{relative_page}: a C++-only page requires runnable C++ examples and no runnable Python examples; "
                    f"found {len(paired['python'])} and {len(paired['cpp'])}"
                )
        elif len(paired["python"]) != len(paired["cpp"]) or not paired["python"]:
            raise ValueError(
                f"{relative_page}: expected the same nonzero number of paired Python and C++ examples; "
                f"found {len(paired['python'])} and {len(paired['cpp'])}"
            )

        libraries = _metadata(source, "example-libraries") or ("bg::library",)
        cmake_source = _source_metadata(source)
        page = relative_page
        title = _title(source, path.stem.replace("-", " ").title())
        examples_for_page = (
            [("", "", False, *cpp) for cpp in paired["cpp"]]
            if cpp_only_marker
            else [(*python, *cpp) for python, cpp in zip(paired["python"], paired["cpp"])]
        )
        for number, (python, python_output, python_ellipsis, cpp, cpp_output, cpp_ellipsis) in enumerate(
            examples_for_page, start=1
        ):
            examples.append(Example(
                name=f"{page}:{number}",
                slug=_slug(page, number),
                page=page,
                title=title,
                python=python.strip(),
                python_output=python_output,
                python_ellipsis=python_ellipsis,
                cpp=cpp.strip(),
                cpp_output=cpp_output,
                cpp_ellipsis=cpp_ellipsis,
                cpp_libraries=libraries,
                cmake_source=cmake_source,
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
        if example.python:
            (downloads / f"{example.slug}.py").write_text(example.python.rstrip() + "\n")
        cmake = [
            "cmake_minimum_required(VERSION 3.31)",
            f"project({example.slug.replace('-', '_')} LANGUAGES CXX)",
            "find_package(bg CONFIG REQUIRED)",
        ]
        if example.cmake_source:
            cmake.extend((
                'set(BUILD_TESTING OFF CACHE BOOL "" FORCE)',
                'if(NOT DEFINED BG_SOURCE_DIR)',
                '    message(FATAL_ERROR "Set BG_SOURCE_DIR to the BG source tree")',
                'endif()',
                f'add_subdirectory("${{BG_SOURCE_DIR}}/{example.cmake_source}" bg_extension)',
            ))
        cmake.extend((
            "add_executable(example main.cpp)",
            "target_link_libraries(example PRIVATE " + " ".join(example.cpp_libraries) + ")",
            "",
        ))
        with ZipFile(downloads / f"{example.slug}.zip", "w", ZIP_DEFLATED) as archive:
            archive.writestr("main.cpp", example.cpp.rstrip() + "\n")
            archive.writestr("CMakeLists.txt", "\n".join(cmake))
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
                    "cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/bg/install"
                    + (" -DBG_SOURCE_DIR=/path/to/bg/source" if example.cmake_source else ""),
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
        python_download = (
            f"{{download}}`Download <../_generated/publish/downloads/{example.slug}.py>`"
            if example.python else "—"
        )
        rows.append(
            f"| [{label}](../{example.page}) | {python_download} "
            f"| {{download}}`Project <../_generated/publish/downloads/{example.slug}.zip>` |"
        )
    (generated / "examples.md").write_text("\n".join(rows) + "\n")

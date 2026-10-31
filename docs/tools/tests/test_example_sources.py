from pathlib import Path
import sys
import tempfile
import unittest
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from example_sources import (collect_examples, expected_output, fences, output_matches,
                             split_cpp_outputs, write_downloads)


class ExampleSourceTests(unittest.TestCase):
    def test_excludes_development_and_repository_documentation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for relative in ("docs/index.md", "docs/development/guide/internal.md"):
                page = root / relative
                page.parent.mkdir(parents=True, exist_ok=True)
                page.write_text("<!-- runnable-example -->\n```python\nprint(1)\n```\n")

            self.assertEqual(collect_examples(root), [])

    def test_collects_marked_homepage(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            docs = root / "docs/user"
            docs.mkdir(parents=True)
            (docs / "index.md").write_text("""# Overview
<!-- runnable-example -->
```python
print(1)
```
Output:
```text
1
```
```cpp
int main() { return 0; }
```
Output:
```text
1
```
""")
            examples = collect_examples(root)
            self.assertEqual([example.page for example in examples], ["index.md"])

    def test_collects_all_pairs_from_supported_page_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            how_to = root / "docs/user/how-to/nested/topic.md"
            reference = root / "docs/user/reference/library/topic.md"
            generated = root / "docs/user/reference/_generated/ignored.md"
            how_to.parent.mkdir(parents=True)
            reference.parent.mkdir(parents=True)
            generated.parent.mkdir(parents=True)
            how_to.write_text("""# Topic

<!-- runnable-example -->

```python
print('first python')
```
Output:
```text
first python
```

```cpp
int main() { return 0; }
```
Output:
```text
first cpp
```

```python
print('second python')
```
Output (ellipsis):
```text
second ...
```

```cpp
int main() { return 0; }
```
Output:
```text
second cpp
```
""")
            reference.write_text("""# Library topic

<!-- runnable-example -->

```python
print(7)
```
Output:
```text
7
```

```cpp
int main() { return 0; }
```
Output:
```text
7
```
""")
            generated.write_text("""# Generated

<!-- runnable-example -->

```python
print(0)
```
Output:
```text
0
```

```cpp
int main() { return 0; }
```
Output:
```text
0
```
""")

            examples = collect_examples(root)

        self.assertEqual([example.page for example in examples], [
            "how-to/nested/topic.md",
            "how-to/nested/topic.md",
            "reference/library/topic.md",
        ])
        self.assertEqual([example.slug for example in examples], [
            "how-to-nested-topic-01",
            "how-to-nested-topic-02",
            "reference-library-topic-01",
        ])
        self.assertEqual([example.python_output for example in examples], [
            "first python", "second ...", "7",
        ])
        self.assertEqual([example.python_ellipsis for example in examples], [False, True, False])
        self.assertEqual([example.cpp_output for example in examples], [
            "first cpp", "second cpp", "7",
        ])
        self.assertEqual([example.title for example in examples], ["Topic", "Topic", "Library topic"])

    def test_collects_cpp_only_extension_examples_and_cmake_metadata(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            page = root / "docs/user/extensions/frame/topic.md"
            page.parent.mkdir(parents=True)
            page.write_text("""# Frame extension

<!-- example-libraries: bg::frame -->
<!-- example-source: extensions/frame -->
<!-- runnable-example: cpp -->

```cpp
int main() { return 0; }
```
Output:
```text
frame ready
```
""")

            examples = collect_examples(root)

        self.assertEqual(len(examples), 1)
        example = examples[0]
        self.assertEqual(example.page, "extensions/frame/topic.md")
        self.assertEqual(example.python, "")
        self.assertEqual(example.python_output, "")
        self.assertEqual(example.cpp, "int main() { return 0; }")
        self.assertEqual(example.cpp_output, "frame ready")
        self.assertEqual(example.cpp_libraries, ("bg::frame",))
        self.assertEqual(example.cmake_source, "extensions/frame")

    def test_cpp_only_page_rejects_python_examples_and_default_marker_stays_paired(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            page = root / "docs/user/extensions/frame/topic.md"
            page.parent.mkdir(parents=True)
            page.write_text("""<!-- runnable-example: cpp -->
```python
print('unexpected')
```
Output:
```text
unexpected
```
```cpp
int main() {}
```
Output:
```text
ok
```
""")
            with self.assertRaisesRegex(ValueError, r"C\+\+-only page"):
                collect_examples(root)

            page.write_text("""<!-- runnable-example -->
```cpp
int main() {}
```
Output:
```text
ok
```
""")
            with self.assertRaisesRegex(ValueError, "same nonzero number"):
                collect_examples(root)

    def test_marked_pages_require_nonzero_matching_pairs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            page = root / "docs/user/guide/empty.md"
            page.parent.mkdir(parents=True)
            page.write_text("""# Empty

<!-- runnable-example -->

Output:
```text
no runnable sources
```
""")

            with self.assertRaisesRegex(ValueError, "same nonzero number"):
                collect_examples(root)

    def test_unpaired_fragments_are_ignored_beside_complete_pairs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            page = root / "docs/user/reference/mixed.md"
            page.parent.mkdir(parents=True)
            page.write_text("""# Mixed

<!-- runnable-example -->

```python
def signature(value: int) -> int: ...
```

```cpp
int signature(int value);
```

```python
print('runnable')
```
Output:
```text
runnable
```

```cpp
int main() { return 0; }
```
Output:
```text
runnable
```
""")

            examples = collect_examples(root)

        self.assertEqual(len(examples), 1)
        self.assertEqual(examples[0].python, "print('runnable')")
        self.assertEqual(examples[0].cpp, "int main() { return 0; }")
        self.assertEqual(examples[0].python_output, "runnable")
        self.assertEqual(examples[0].cpp_output, "runnable")

    def test_unequal_nonzero_paired_counts_fail_with_unpaired_fragments(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            page = root / "docs/user/reference/unequal.md"
            page.parent.mkdir(parents=True)
            page.write_text("""# Unequal

<!-- runnable-example -->

```python
print(1)
```
Output:
```text
1
```

```python
print(2)
```
Output:
```text
2
```

```cpp
int main() { return 0; }
```
Output:
```text
1
```

```cpp
int signature(int value);
```
""")

            with self.assertRaisesRegex(ValueError, "found 2 and 1"):
                collect_examples(root)

    def test_output_pair_does_not_skip_unrelated_fences(self):
        source = """```python
print(1)
```

This text separates the code from its output.

```cpp
int main() {}
```

Output:

```text
true
```
"""
        blocks = fences(source)
        self.assertIsNone(expected_output(source, blocks[0], blocks))
        self.assertEqual(expected_output(source, blocks[1], blocks), ("true", False))

    def test_ellipsis_requires_explicit_output_label(self):
        source = """```python
print('elapsed: 12 ns')
```

Output (ellipsis):

```text
elapsed: ... ns
```
"""
        blocks = fences(source)
        expected = expected_output(source, blocks[0], blocks)
        self.assertEqual(expected, ("elapsed: ... ns", True))
        self.assertTrue(output_matches(expected[0], "elapsed: 12 ns", expected[1]))
        self.assertFalse(output_matches("elapsed: ... ns", "elapsed: 12 ns"))
        self.assertFalse(output_matches("elapsed: 12 ns", "elapsed: 13 ns", False))

    def test_cpp_batch_checks_each_output_segment_independently(self):
        stdout = "elapsed: 12 ns\n\nBG_DOC_EXAMPLE_4\nready\nBG_DOC_EXAMPLE_5\n"
        actual = split_cpp_outputs(stdout, 4, 2)
        self.assertTrue(output_matches("elapsed: ... ns", actual[0], True))
        self.assertTrue(output_matches("ready", actual[1]))

    def test_collected_examples_have_stable_individual_sources(self):
        root = Path(__file__).resolve().parents[3]
        examples = collect_examples(root)
        self.assertTrue(examples)
        self.assertEqual(len({example.slug for example in examples}), len(examples))
        for example in examples:
            if not example.python:
                self.assertTrue(example.page.startswith("extensions/"), example.name)
            self.assertTrue(example.cpp)
            self.assertIn("int main()", example.cpp, example.name)

    def test_downloads_are_complete_cpp_projects(self):
        example = collect_examples(Path(__file__).resolve().parents[3])[0]
        with tempfile.TemporaryDirectory() as temporary:
            docs = Path(temporary) / "docs"
            write_downloads([example], docs)

            python = docs / "_generated/publish/downloads" / f"{example.slug}.py"
            archive_path = docs / "_generated/publish/downloads" / f"{example.slug}.zip"
            catalog = (docs / "_generated/examples.md").read_text()
            self.assertEqual(python.read_text(), example.python + "\n")
            self.assertIn(f"{example.slug}.py", catalog)
            self.assertIn(f"{example.slug}.zip", catalog)
            self.assertTrue(catalog.startswith("| Example | Python (.py) | C++ project (.zip) |"))
            self.assertIn(f"../{example.page}", catalog)
            self.assertIn(f"{{download}}`Download <../_generated/publish/downloads/{example.slug}.py>`", catalog)
            with ZipFile(archive_path) as archive:
                self.assertEqual(set(archive.namelist()), {"main.cpp", "CMakeLists.txt", "README.md"})
                self.assertEqual(archive.read("main.cpp").decode(), example.cpp + "\n")
                cmake = archive.read("CMakeLists.txt").decode()
                self.assertIn("find_package(bg CONFIG REQUIRED)", cmake)
                self.assertIn("target_link_libraries(example PRIVATE bg::library)", cmake)
                self.assertIn("## Build and run", archive.read("README.md").decode())

    def test_downloads_skip_python_and_include_extension_cmake_dependencies(self):
        from example_sources import Example

        example = Example(
            name="extensions/frame/topic.md:1",
            slug="extensions-frame-topic-01",
            page="extensions/frame/topic.md",
            title="Frame extension",
            python="",
            python_output="",
            python_ellipsis=False,
            cpp="int main() { return 0; }",
            cpp_output="frame ready",
            cpp_ellipsis=False,
            cpp_libraries=("bg::frame",),
            cmake_source="extensions/frame",
        )
        with tempfile.TemporaryDirectory() as temporary:
            docs = Path(temporary) / "docs"
            write_downloads([example], docs)

            downloads = docs / "_generated/publish/downloads"
            self.assertFalse((downloads / f"{example.slug}.py").exists())
            catalog = (docs / "_generated/examples.md").read_text()
            self.assertIn("| — |", catalog)
            self.assertNotIn(".py`", catalog)
            with ZipFile(downloads / f"{example.slug}.zip") as archive:
                cmake = archive.read("CMakeLists.txt").decode()
                self.assertIn("add_subdirectory(\"${BG_SOURCE_DIR}/extensions/frame\" bg_extension)", cmake)
                self.assertIn("target_link_libraries(example PRIVATE bg::frame)", cmake)


if __name__ == "__main__":
    unittest.main()

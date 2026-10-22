from pathlib import Path
import sys
import tempfile
import unittest
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from example_sources import (collect_examples, expected_output, fences, output_matches,
                             split_cpp_outputs, write_downloads)


class ExampleSourceTests(unittest.TestCase):
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
        root = Path(__file__).resolve().parents[2]
        examples = collect_examples(root)
        self.assertGreater(len(examples), 50)
        self.assertEqual(len({example.slug for example in examples}), len(examples))
        for example in examples:
            self.assertTrue(example.python)
            self.assertTrue(example.cpp)
            self.assertIn("int main()", example.cpp, example.name)

    def test_downloads_are_complete_cpp_projects(self):
        example = collect_examples(Path(__file__).resolve().parents[2])[0]
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


if __name__ == "__main__":
    unittest.main()

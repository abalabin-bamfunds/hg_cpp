"""Run the published examples and optionally build their C++ counterparts."""

from pathlib import Path
import argparse
import subprocess
import sys

from example_sources import collect_examples, output_matches, split_cpp_outputs


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cmake-prefix", type=Path)
parser.add_argument("--cmake", default="cmake")
parser.add_argument("--jobs", type=int, default=2)
parser.add_argument("--cpp-compiler", help="C++23 compiler passed to CMake")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
project = root / "build/doc-examples"
project.mkdir(parents=True, exist_ok=True)
examples = collect_examples(root)

for index, example in enumerate(examples):
    path = project / f"example_{index}.py"
    path.write_text(example.python + "\n")
    result = subprocess.run([sys.executable, str(path)], text=True, capture_output=True)
    if result.returncode or not output_matches(example.python_output, result.stdout, example.python_ellipsis):
        raise RuntimeError(
            f"Python {example.name}: expected {example.python_output!r}, got {result.stdout.strip()!r}"
            f"\n{result.stderr}"
        )
    print(f"Python {example.name}: passed", flush=True)

cmake = [
    "cmake_minimum_required(VERSION 3.31)",
    "project(documentation_examples LANGUAGES CXX)",
    "find_package(bg CONFIG REQUIRED)",
]
batches = []
for start in range(0, len(examples), 8):
    batch = examples[start:start + 8]
    includes = {"#include <iostream>"}
    bodies = []
    calls = []
    for offset, example in enumerate(batch):
        index = start + offset
        lines = []
        for line in example.cpp.splitlines():
            if line.startswith("#include"):
                includes.add(line)
            else:
                lines.append(line)
        body = "\n".join(lines).replace("int main()", "void run()")
        bodies.append(f"namespace example_{index} {{\n{body}\n}}\n")
        calls.extend((
            f"    example_{index}::run();",
            f'    std::cout << "\\nBG_DOC_EXAMPLE_{index}\\n";'
        ))

    target = f"examples_{start // 8}"
    source = "\n".join(sorted(includes)) + "\n\n" + "\n".join(bodies)
    source += "\nint main() {\n" + "\n".join(calls) + "\n}\n"
    source_path = project / f"{target}.cpp"
    if not source_path.exists() or source_path.read_text() != source:
        source_path.write_text(source)
    cmake.extend([f"add_executable({target} {target}.cpp)",
                  f"target_link_libraries({target} PRIVATE bg::library)"])
    batches.append((target, batch, start))

(project / "CMakeLists.txt").write_text("\n".join(cmake) + "\n")
print(f"Validated {len(examples)} Python examples; extracted {len(examples)} C++ examples.", flush=True)

if args.cmake_prefix:
    build = project / "build"
    configure = [args.cmake, "-S", str(project), "-B", str(build),
                 "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS_DEBUG=-O0 -g0",
                 f"-DCMAKE_PREFIX_PATH={args.cmake_prefix.resolve()}"]
    if args.cpp_compiler:
        configure.append(f"-DCMAKE_CXX_COMPILER={args.cpp_compiler}")
    subprocess.run(configure, check=True)
    subprocess.run([args.cmake, "--build", str(build), "-j", str(args.jobs)], check=True)

    failures = []
    for target, batch, start in batches:
        result = subprocess.run([str(build / target)], text=True, capture_output=True)
        try:
            outputs = split_cpp_outputs(result.stdout, start, len(batch))
        except ValueError as error:
            failures.append(f"{target}: {error}\n{result.stdout}{result.stderr}")
            continue
        for example, output in zip(batch, outputs):
            if result.returncode or not output_matches(example.cpp_output, output, example.cpp_ellipsis):
                failures.append(
                    f"{example.name}: expected {example.cpp_output!r}, got {output.strip()!r}\n{result.stderr}"
                )
    if failures:
        raise RuntimeError("\n\n".join(failures))
    print(f"All {len(examples)} C++ examples passed.", flush=True)

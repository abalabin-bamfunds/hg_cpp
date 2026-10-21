"""Run the published examples and optionally build their C++ counterparts."""

from pathlib import Path
import argparse
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmake-prefix', type=Path)
parser.add_argument('--cmake', default='cmake')
parser.add_argument('--jobs', type=int, default=2)
parser.add_argument('--cpp-compiler', help='C++23 compiler passed to CMake')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
project = root / 'build/doc-examples'
project.mkdir(parents=True, exist_ok=True)

def blocks(page, language):
    return re.findall(r'```' + language + r'\n(.*?)\n```', (root / 'docs/guide' / page).read_text(), re.S)

python_examples = []
cpp_examples = []
for page, expected in (
    ('getting-started.md', '[18]'),
    ('arithmetic.md', '[200]'),
    ('connect-calculations.md', '[200]'),
):
    python_examples.append((page, blocks(page, 'python')[0], expected))
    cpp_examples.append((page, blocks(page, 'cpp')[0]))

py = blocks('changing-values.md', 'python')
cpp = blocks('changing-values.md', 'cpp')
for i, expected in enumerate(('[200, 220, 210]', '[200, None, 200, 210]', '[None, 200, 210, 220]', '[200, 220]')):
    code = py[i]
    if i in (1, 3):
        base = py[0 if i == 1 else 2]
        code = base[:base.index('print(test_graph(')] + code
    python_examples.append((f'changing_values_{i}', code, expected))
    code = cpp[i]
    if i in (1, 3):
        base = cpp[0 if i == 1 else 2]
        code = base[:base.index('int main()')] + 'int main() {\n' + code + '\n}\n'
    cpp_examples.append((f'changing_values_{i}', code))

for i, expected in ((0, '[20, 21, 21]'), (2, '[20, 22, 21]'), (3, '[False, True, True, False]'), (4, '[-1, 0, 1, -1]')):
    python_examples.append((f'conditions_{i}', blocks('conditions.md', 'python')[i], expected))
    cpp_examples.append((f'conditions_{i}', blocks('conditions.md', 'cpp')[i]))

py = blocks('dedup.md', 'python')
cpp = blocks('dedup.md', 'cpp')
for i in range(2):
    code = py[i]
    if i == 1:
        code = py[0][:py[0].index('@graph')] + code + '\n\n' + py[0][py[0].index('print(test_graph('):]
    python_examples.append((f'dedup_{i}', code, '[20, None, None, 21, 20, None]'))
    code = cpp[i]
    if i == 1:
        code = cpp[0][:cpp[0].index('auto reported_temperature')] + code + '\n\n' + cpp[0][cpp[0].index('int main()'):]
    cpp_examples.append((f'dedup_{i}', code))

for page, expected_outputs in (
    ('filter.md', ['[None, 20, None, None, 22, None, None, 22]']),
    ('sample.md', ['[None, 21, 21, None, 23, 23]']),
    ('count-and-lag.md', ['[None, 1, 2, None, 3, 4]', '[None, None, 20, None, 20, 26]']),
    ('running-calculations.md', ['[None, 20, 40, None, 66, 90]', '[None, 20.0, 20.0, None, 22.0, 22.5]']),
):
    for i, expected in enumerate(expected_outputs):
        python_examples.append((f'{page}_{i}', blocks(page, 'python')[i], expected))
        cpp_examples.append((f'{page}_{i}', blocks(page, 'cpp')[i]))


cpp_expected = ['true'] * len(cpp_examples)
for path in sorted((root / 'docs').rglob('*.md')):
    source = path.read_text()
    if '<!-- runnable-example -->' not in source.splitlines():
        continue
    python_blocks = re.findall(r'```python\n(.*?)\n```\n\nOutput:\n\n```text\n(.*?)\n```', source, re.S)
    native_blocks = re.findall(r'```cpp\n(.*?)\n```\n\nOutput:\n\n```text\n(.*?)\n```', source, re.S)
    assert len(python_blocks) == len(native_blocks) > 0, path
    for index, (python_block, native_block) in enumerate(zip(python_blocks, native_blocks)):
        name = f'{path.relative_to(root)}:{index + 1}'
        python_examples.append((name, *python_block))
        cpp_examples.append((name, native_block[0]))
        cpp_expected.append(native_block[1])

for index, (name, code, expected) in enumerate(python_examples):
    path = project / f'example_{index}.py'
    path.write_text(code + '\n')
    result = subprocess.run([sys.executable, str(path)], text=True, capture_output=True)
    if result.returncode or result.stdout.strip() != expected:
        raise RuntimeError(f'{name}: expected {expected!r}, got {result.stdout!r}\n{result.stderr}')
    print(f'Python {name}: passed', flush=True)

cmake = [
    'cmake_minimum_required(VERSION 3.31)',
    'project(documentation_examples LANGUAGES CXX)',
    'find_package(bg CONFIG REQUIRED)',
    'enable_testing()',
]
batches = []
for start in range(0, len(cpp_examples), 8):
    includes = set()
    bodies = []
    calls = []
    for offset, (name, code) in enumerate(cpp_examples[start:start + 8]):
        index = start + offset
        lines = []
        for line in code.splitlines():
            if line.startswith('#include'):
                includes.add(line)
            else:
                lines.append(line)
        body = '\n'.join(lines).replace('int main()', 'void run()')
        bodies.append(f'namespace example_{index} {{\n{body}\n}}\n')
        calls.append(f'    example_{index}::run();')
    target = f'examples_{start // 8}'
    source = '\n'.join(sorted(includes)) + '\n\n' + '\n'.join(bodies)
    source += '\nint main() {\n' + '\n'.join(calls) + '\n}\n'
    path = project / f'{target}.cpp'
    if not path.exists() or path.read_text() != source:
        path.write_text(source)
    cmake.extend([f'add_executable({target} {target}.cpp)', f'target_link_libraries({target} PRIVATE bg::library)'])
    batches.append((target, cpp_expected[start:start + 8]))
(project / 'CMakeLists.txt').write_text('\n'.join(cmake) + '\n')
print(f'Validated {len(python_examples)} Python examples; extracted {len(cpp_examples)} C++ examples.', flush=True)

if args.cmake_prefix:
    build = project / 'build'
    configure = [args.cmake, '-S', str(project), '-B', str(build),
                 '-DCMAKE_BUILD_TYPE=Debug', '-DCMAKE_CXX_FLAGS_DEBUG=-O0 -g0',
                 f'-DCMAKE_PREFIX_PATH={args.cmake_prefix.resolve()}']
    if args.cpp_compiler:
        configure.append(f'-DCMAKE_CXX_COMPILER={args.cpp_compiler}')
    subprocess.run(configure, check=True)
    subprocess.run([args.cmake, '--build', str(build), '-j', str(args.jobs)], check=True)
    failures = []
    for target, expected in batches:
        result = subprocess.run([str(build / target)], text=True, capture_output=True)
        if result.returncode or result.stdout.strip().splitlines() != expected:
            failures.append(f'{target}: exit {result.returncode}; expected {expected!r}\n'
                            f'{result.stdout}{result.stderr}')
    if failures:
        raise RuntimeError('\n\n'.join(failures))
    print(f'All {len(cpp_examples)} C++ examples passed.', flush=True)

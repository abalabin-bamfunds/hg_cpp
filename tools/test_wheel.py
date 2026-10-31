#!/usr/bin/env python3
"""Install a wheel in a temporary virtual environment and run a graph smoke test."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import tomllib
import venv


SMOKE_TEST = r'''
from importlib.metadata import distribution
import os
from pathlib import Path
from sysconfig import get_paths
import bg

package_dir = Path(bg.__file__).resolve().parent
assert package_dir.is_relative_to(Path(get_paths()["purelib"]).resolve())
assert distribution(os.environ["BG_DISTRIBUTION_NAME"]).version == os.environ["BG_DISTRIBUTION_VERSION"]

@bg.node
def increment(value: int) -> int:
    return value + 1

@bg.graph
def graph(value: int) -> int:
    return increment(value)

assert bg.test_graph(graph, [1, 4, None, 7]) == [2, 5, None, 8]
print(f"Imported bg from {package_dir}; graph smoke passed")
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheel", type=Path)
    args = parser.parse_args()
    wheel = args.wheel.resolve()
    project = tomllib.loads(
        (Path(__file__).resolve().parents[1] / "pyproject.toml").read_text()
    )["project"]
    environment_variables = os.environ.copy()
    for name in ("PYTHONPATH", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        environment_variables.pop(name, None)
    environment_variables["PIP_CONFIG_FILE"] = os.devnull
    environment_variables["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"

    with tempfile.TemporaryDirectory(prefix="bg-wheel-smoke-") as temporary_directory:
        temporary_path = Path(temporary_directory)
        environment = temporary_path / "venv"
        venv.EnvBuilder(with_pip=True).create(environment)
        python = environment / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
        subprocess.run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "--no-index",
                "--no-deps",
                "--disable-pip-version-check",
                str(wheel),
            ],
            check=True,
            env=environment_variables,
        )
        smoke_file = temporary_path / "smoke.py"
        smoke_file.write_text(SMOKE_TEST, encoding="utf-8")
        environment_variables["BG_DISTRIBUTION_NAME"] = project["name"]
        environment_variables["BG_DISTRIBUTION_VERSION"] = project["version"]
        subprocess.run(
            [str(python), "-I", str(smoke_file)],
            cwd=temporary_path,
            env=environment_variables,
            check=True,
        )


if __name__ == "__main__":
    main()

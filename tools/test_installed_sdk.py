#!/usr/bin/env python3
"""Build and run native consumers against the installed wheel SDK."""

from __future__ import annotations

from importlib import metadata
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import tomllib


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    project = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]
    distribution = metadata.distribution(project["name"])
    install_prefix = Path(distribution.locate_file("")).resolve()
    package_dir = install_prefix / "hg"

    with tempfile.TemporaryDirectory(prefix="hg-sdk-consumer-") as temporary_directory:
        build_directory = Path(temporary_directory) / "build"
        subprocess.run(
            [
                "cmake",
                "-S",
                str(ROOT / "packaging/consumer"),
                "-B",
                str(build_directory),
                "-DCMAKE_BUILD_TYPE=Release",
                f"-DCMAKE_PREFIX_PATH={install_prefix}",
                f"-DPython_EXECUTABLE={sys.executable}",
                f"-DHG_TEST_PACKAGE_DIR={package_dir}",
            ],
            check=True,
        )
        subprocess.run(["cmake", "--build", str(build_directory), "--parallel", "2"], check=True)
        executable = build_directory / "hg_sdk_consumer"
        if os.name == "nt":
            executable = build_directory / "hg_sdk_consumer.exe"
        runtime_environment = os.environ.copy()
        if os.name == "nt":
            runtime_environment["PATH"] = os.pathsep.join(
                (
                    str(package_dir),
                    str(install_prefix / "bin"),
                    str(install_prefix / f"{project['name']}.libs"),
                    runtime_environment["PATH"],
                )
            )
        subprocess.run([str(executable)], check=True, env=runtime_environment)
        subprocess.run(
            [
                sys.executable,
                "-c",
                "import hg; import hg_sdk_python_consumer; assert hg_sdk_python_consumer.version()",
            ],
            cwd=build_directory,
            check=True,
            env=runtime_environment,
        )
        print(f"Built and ran installed HG SDK consumers from {install_prefix}")


if __name__ == "__main__":
    main()

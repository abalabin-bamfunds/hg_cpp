#!/usr/bin/env python3
"""Build and run native consumers against an installed HG SDK."""

from __future__ import annotations

import argparse
from importlib import metadata
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import tomllib
from collections.abc import Sequence


ROOT = Path(__file__).resolve().parents[1]


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, help="test an installed CMake prefix instead of an installed wheel")
    parser.add_argument("--web", action="store_true", help="also build and run the installed hg::web consumer")
    parsed = parser.parse_args(arguments)
    if parsed.prefix is not None:
        parsed.prefix = parsed.prefix.resolve()
        if not parsed.prefix.is_dir():
            parser.error(f"install prefix does not exist: {parsed.prefix}")
    return parsed


def main(arguments: Sequence[str] | None = None) -> None:
    options = parse_arguments(arguments)
    project = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]
    if options.prefix is None:
        distribution = metadata.distribution(project["name"])
        install_prefix = Path(distribution.locate_file("")).resolve()
    else:
        install_prefix = options.prefix
    package_dir = install_prefix / "hgraph"
    if options.web and not (install_prefix / "hg_web").is_dir():
        raise FileNotFoundError(f"installed prefix does not contain hg_web: {install_prefix}")

    with tempfile.TemporaryDirectory(prefix="hg-sdk-consumer-") as temporary_directory:
        build_directory = Path(temporary_directory) / "build"
        configure_command = [
            "cmake",
            "-S",
            str(ROOT / "packaging/consumer"),
            "-B",
            str(build_directory),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_PREFIX_PATH={install_prefix}",
            f"-DPython_EXECUTABLE={sys.executable}",
            f"-DHG_TEST_WEB={'ON' if options.web else 'OFF'}",
        ]
        if options.prefix is None:
            configure_command.append(f"-DHG_TEST_PACKAGE_DIR={package_dir}")
        subprocess.run(configure_command, check=True)
        subprocess.run(["cmake", "--build", str(build_directory), "--parallel", "2"], check=True)
        executable = build_directory / "hg_sdk_consumer"
        if os.name == "nt":
            executable = build_directory / "hg_sdk_consumer.exe"
        runtime_environment = os.environ.copy()
        if options.prefix is not None:
            runtime_environment["PYTHONPATH"] = str(install_prefix)
        if os.name == "nt":
            runtime_paths = [str(package_dir), str(install_prefix / "bin")]
            if options.web:
                runtime_paths.append(str(install_prefix / "hg_web"))
            if options.prefix is None:
                runtime_paths.append(str(install_prefix / f"{project['name'].replace('-', '_')}.libs"))
            runtime_environment["PATH"] = os.pathsep.join((*runtime_paths, runtime_environment["PATH"]))
        python_dll_directory = runtime_environment.get("HG_TEST_DLL_DIR")
        python_dll_setup = (
            f"import os; _hg_dll_directory = os.add_dll_directory({python_dll_directory!r}); "
            if os.name == "nt" and python_dll_directory
            else ""
        )
        subprocess.run([str(executable)], check=True, env=runtime_environment)
        if options.web:
            web_executable = build_directory / "hg_web_sdk_consumer"
            if os.name == "nt":
                web_executable = web_executable.with_suffix(".exe")
            subprocess.run([str(web_executable)], check=True, env=runtime_environment)
            import_check = (
                python_dll_setup
                + "from pathlib import Path; import hg_web, hgraph; "
                f"prefix = Path({str(install_prefix)!r}).resolve(); "
                "assert Path(hg_web.__file__).resolve().is_relative_to(prefix); "
                "assert Path(hgraph.__file__).resolve().is_relative_to(prefix)"
            )
            subprocess.run(
                [sys.executable, "-c", import_check],
                cwd=build_directory,
                check=True,
                env=runtime_environment,
            )
        subprocess.run(
            [
                sys.executable,
                "-c",
                python_dll_setup
                + "import hgraph; import hg_sdk_python_consumer; assert hg_sdk_python_consumer.version()",
            ],
            cwd=build_directory,
            check=True,
            env=runtime_environment,
        )
        print(f"Built and ran installed HG SDK consumers from {install_prefix}")


if __name__ == "__main__":
    main()

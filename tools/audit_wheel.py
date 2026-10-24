#!/usr/bin/env python3
"""Check that a bg wheel contains its Python package and native SDK."""

from __future__ import annotations

import argparse
from email.parser import Parser
from pathlib import Path, PurePosixPath
import re
import tomllib
import zipfile


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_PARTS = {".git", "__pycache__", ".pytest_cache", ".mypy_cache"}


def canonicalize(name: str) -> str:
    return re.sub(r"[-_.]+", "-", name).lower()


def audit(path: Path) -> int:
    project = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]
    distribution = canonicalize(project["name"])
    version = project["version"]

    with zipfile.ZipFile(path) as wheel:
        members = [info for info in wheel.infolist() if not info.is_dir()]
        paths = {info.filename for info in members}

        for name in paths:
            path_parts = PurePosixPath(name)
            if path_parts.is_absolute() or ".." in path_parts.parts:
                raise ValueError(f"unsafe wheel path: {name}")
            if any(part in FORBIDDEN_PARTS for part in path_parts.parts):
                raise ValueError(f"wheel contains a development-only path: {name}")
            if any(part.startswith((".venv", "cmake-build")) for part in path_parts.parts):
                raise ValueError(f"wheel contains a development-only path: {name}")
            if path_parts.suffix in {".pyc", ".pyo"}:
                raise ValueError(f"wheel contains bytecode: {name}")

        metadata_paths = [name for name in paths if name.endswith(".dist-info/METADATA")]
        if len(metadata_paths) != 1:
            raise ValueError(f"expected one wheel METADATA file, found {metadata_paths}")
        metadata = Parser().parsestr(wheel.read(metadata_paths[0]).decode("utf-8"))
        if canonicalize(metadata["Name"]) != distribution or metadata["Version"] != version:
            raise ValueError(
                f"wheel metadata identifies {metadata['Name']} {metadata['Version']}; "
                f"pyproject.toml identifies {project['name']} {version}"
            )

        required = {
            "bg/__init__.py",
            "bg/operators.py",
            "include/bg/bg.hpp",
        }
        missing = required - paths
        if missing:
            raise ValueError("wheel is missing required files: " + ", ".join(sorted(missing)))

        cmake_roots = ("lib/cmake/bg", "lib64/cmake/bg")
        if not any(
            all(
                f"{root}/{name}" in paths
                for name in ("bgConfig.cmake", "bgConfigVersion.cmake", "bgTargets.cmake")
            )
            for root in cmake_roots
        ):
            raise ValueError("wheel is missing the BG CMake package under lib or lib64")

        if not any(
            f"{root}/cmake/ddvisitor/ddvisitor-config.cmake" in paths
            for root in ("lib", "lib64")
        ):
            raise ValueError("wheel is missing the ddvisitor CMake dependency")

        if not any(
            name.startswith("bg/_bg") and name.endswith((".so", ".pyd"))
            for name in paths
        ):
            raise ValueError("wheel is missing the bg._bg native extension")

        for library in ("libbg", "libbg_library", "libbg_python", "libnanobind"):
            if not any(
                PurePosixPath(name).parent in (PurePosixPath("lib"), PurePosixPath("lib64"))
                and PurePosixPath(name).name.startswith(library + ".")
                for name in paths
            ):
                raise ValueError(f"wheel is missing the native library {library}")

        print(f"Audited {path}: {len(paths)} files, {project['name']} {version}")
        return len(paths)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wheel", type=Path)
    args = parser.parse_args()
    audit(args.wheel)


if __name__ == "__main__":
    main()

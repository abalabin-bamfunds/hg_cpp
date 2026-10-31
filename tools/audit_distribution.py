#!/usr/bin/env python3
"""Check required files and metadata in hg_cpp wheel and source distributions."""

from __future__ import annotations

import argparse
from email.parser import Parser
from pathlib import Path, PurePosixPath
import tarfile
import tomllib
import zipfile

from packaging.tags import Tag
from packaging.utils import canonicalize_name, parse_wheel_filename


ROOT = Path(__file__).resolve().parents[1]
FORBIDDEN_PARTS = {".git", "__pycache__", ".pytest_cache", ".mypy_cache"}
WHEEL_FILES = {
    "bg/__init__.py",
    "bg/operators.py",
    "include/bg/bg.hpp",
}
SDIST_FILES = {
    "CMakeLists.txt",
    "bindings/python/CMakeLists.txt",
    "bindings/python/python/bg/__init__.py",
    "bindings/python/src/module.cpp",
    "bindings/python/tests/extension_module.cpp",
    "cmake/bgConfig.cmake.in",
    "include/bg/bg.hpp",
    "packaging/consumer/CMakeLists.txt",
    "pyproject.toml",
    "src/version.cpp",
}


def check_paths(paths: set[str]) -> None:
    for name in paths:
        path = PurePosixPath(name)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"unsafe archive path: {name}")
        if any(part in FORBIDDEN_PARTS for part in path.parts):
            raise ValueError(f"archive contains a development-only path: {name}")
        if path.suffix in {".pyc", ".pyo"}:
            raise ValueError(f"archive contains Python bytecode: {name}")


def audit_wheel(path: Path, platform: str | None) -> None:
    project = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]
    wheel_name, wheel_version, _, tags = parse_wheel_filename(path.name)
    if canonicalize_name(wheel_name) != canonicalize_name(project["name"]):
        raise ValueError(f"wheel name {wheel_name} does not match pyproject.toml")
    if str(wheel_version) != project["version"]:
        raise ValueError(f"wheel version {wheel_version} does not match pyproject.toml")
    expected_tag = Tag("cp312", "abi3", platform) if platform else None

    with zipfile.ZipFile(path) as wheel:
        members = [info for info in wheel.infolist() if not info.is_dir()]
        paths = {info.filename for info in members}
        check_paths(paths)

        metadata_paths = [name for name in paths if name.endswith(".dist-info/METADATA")]
        if len(metadata_paths) != 1:
            raise ValueError(f"expected one wheel METADATA file, found {metadata_paths}")
        metadata = Parser().parsestr(wheel.read(metadata_paths[0]).decode("utf-8"))
        if canonicalize_name(metadata["Name"]) != canonicalize_name(project["name"]):
            raise ValueError(f"wheel METADATA has unexpected name {metadata['Name']}")
        if metadata["Version"] != project["version"]:
            raise ValueError(f"wheel METADATA has unexpected version {metadata['Version']}")

        missing = WHEEL_FILES - paths
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

        if not any(name.startswith("bg/_bg") and name.endswith((".so", ".pyd")) for name in paths):
            raise ValueError("wheel is missing the bg._bg native extension")

        for library in ("bg", "bg_library", "bg_python", "nanobind", "fmt"):
            if not any(is_runtime_library(name, library) for name in paths):
                raise ValueError(f"wheel is missing the native runtime library {library}")

        if not any("cmake/fmt/" in name and name.endswith("fmt-config.cmake") for name in paths):
            raise ValueError("wheel is missing the fmt CMake package required by the installed SDK")
        if not any(name.startswith("include/fmt/") for name in paths):
            raise ValueError("wheel is missing fmt headers required by public BG headers")

    if expected_tag is not None and expected_tag not in tags:
        raise ValueError(f"wheel has no {expected_tag} tag: {path.name} contains {sorted(tags)}")
    if any(tag.abi != "abi3" for tag in tags):
        raise ValueError(f"wheel is not stable-ABI tagged: {path.name} contains {sorted(tags)}")

    print(f"Audited wheel {path}: {len(paths)} files, {project['name']} {project['version']}")


def is_runtime_library(path: str, library: str) -> bool:
    name = PurePosixPath(path).name.lower()
    base = name.removeprefix("lib")
    prefix = library.lower()
    matches_library = base == prefix or base.startswith((f"{prefix}.", f"{prefix}-"))
    has_library_suffix = name.endswith((".dll", ".dylib", ".a", ".lib")) or ".so" in name
    return matches_library and has_library_suffix


def audit_sdist(path: Path) -> None:
    project = tomllib.loads((ROOT / "pyproject.toml").read_text())["project"]
    with tarfile.open(path, "r:gz") as archive:
        archive_members = archive.getmembers()
        archive_paths = {PurePosixPath(member.name) for member in archive_members}
        check_paths({str(member_path) for member_path in archive_paths})
        roots = {member_path.parts[0] for member_path in archive_paths if member_path.parts}
        if len(roots) != 1:
            raise ValueError(f"sdist must contain one top-level directory, found {sorted(roots)}")
        archive_root = next(iter(roots))
        expected_root = f"{project['name'].replace('-', '_')}-{project['version']}"
        if archive_root != expected_root:
            raise ValueError(f"sdist root {archive_root} does not match {expected_root}")
        if any(member.issym() or member.islnk() for member in archive_members):
            raise ValueError("sdist must not contain symbolic or hard links")

        members = [member for member in archive_members if member.isfile()]
        paths = {"/".join(PurePosixPath(member.name).parts[1:]) for member in members}
        check_paths(paths)
        missing = SDIST_FILES - paths
        if missing:
            raise ValueError("sdist is missing required files: " + ", ".join(sorted(missing)))

        pyproject = next(member for member in members if member.name == f"{archive_root}/pyproject.toml")
        project_data = tomllib.loads(archive.extractfile(pyproject).read().decode("utf-8"))["project"]
        if project_data["name"] != project["name"] or project_data["version"] != project["version"]:
            raise ValueError("sdist pyproject.toml metadata does not match the checkout")

    print(f"Audited sdist {path}: {len(paths)} files, {project['name']} {project['version']}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--platform", help="Require the cp312-abi3 wheel tag for this platform")
    args = parser.parse_args()

    if args.archive.is_dir():
        archives = sorted((*args.archive.glob("*.whl"), *args.archive.glob("*.tar.gz")))
        if not archives:
            raise FileNotFoundError(f"no wheel or sdist found in {args.archive}")
    else:
        archives = [args.archive]

    for archive in archives:
        if archive.name.endswith(".whl"):
            audit_wheel(archive, args.platform)
        elif archive.name.endswith(".tar.gz"):
            audit_sdist(archive)
        else:
            raise ValueError(f"unsupported distribution archive: {archive}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3

"""Generate a test coverage report from structured Catch2 tags."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from collections import Counter, defaultdict
from itertools import product
from pathlib import Path


TAG_VALUE = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("test_executable", type=Path)
    parser.add_argument("--dimensions", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true")
    return parser.parse_args()


def read_dimensions(path: Path) -> dict[str, set[str]]:
    lines = path.read_text().splitlines()
    start = lines.index("Use `[dimension:value]` for each applicable dimension:") + 1
    end = lines.index("Use lower-case kebab-case values in tags. For example:")
    dimensions: dict[str, set[str]] = {}
    current: str | None = None

    for line in lines[start:end]:
        match = re.match(r"- `([^`]+)`:", line)
        if match:
            current = match.group(1)
            dimensions[current] = set(re.findall(r"`([^`]+)`", line)[1:])
        elif current:
            dimensions[current].update(re.findall(r"`([^`]+)`", line))

    if not dimensions or any(not values for values in dimensions.values()):
        raise ValueError(f"could not read coverage dimensions from {path}")

    return dimensions


def read_tests(executable: Path) -> list[dict[str, object]]:
    result = subprocess.run(
        [str(executable), "--list-tests", "--reporter", "JSON"],
        check=True,
        capture_output=True,
        text=True,
    )
    return json.loads(result.stdout)["listings"]["tests"]


def structured_tags(tags: list[str]) -> dict[str, set[str]]:
    result: dict[str, set[str]] = defaultdict(set)
    for tag in tags:
        if ":" in tag:
            dimension, value = tag.split(":", 1)
            result[dimension].add(value)
    return result


def validate_tests(tests: list[dict[str, object]], dimensions: dict[str, set[str]]) -> list[str]:
    errors: list[str] = []
    for test in tests:
        name = str(test["name"])
        tags = [str(tag) for tag in test["tags"]]
        coverage = structured_tags(tags)

        if "coverage-exempt" in tags:
            if coverage:
                errors.append(f"{name}: [coverage-exempt] cannot be combined with coverage dimensions")
            continue
        if not coverage:
            errors.append(f"{name}: missing coverage tags or [coverage-exempt]")
            continue

        if coverage and "feature" not in coverage:
            errors.append(f"{name}: coverage dimensions require a feature tag")

        for dimension, values in coverage.items():
            if dimension not in dimensions:
                errors.append(f"{name}: unknown coverage dimension {dimension!r}")
                continue
            for value in values:
                if not TAG_VALUE.fullmatch(value):
                    errors.append(f"{name}: invalid {dimension} value {value!r}")
                elif value not in dimensions[dimension]:
                    errors.append(f"{name}: undocumented {dimension} value {value!r}")

    return errors


def markdown_report(tests: list[dict[str, object]], dimensions: dict[str, set[str]], errors: list[str]) -> str:
    feature_tests: dict[str, list[dict[str, set[str]]]] = defaultdict(list)
    observed_values: dict[str, set[str]] = defaultdict(set)
    exempt = 0
    unclassified = 0

    for test in tests:
        tags = [str(tag) for tag in test["tags"]]
        coverage = structured_tags(tags)
        if "coverage-exempt" in tags:
            exempt += 1
        elif not coverage:
            unclassified += 1
        for dimension, values in coverage.items():
            observed_values[dimension].update(values)
        for feature in coverage.get("feature", ()):
            feature_tests[feature].append(coverage)

    lines = [
        "# Test coverage",
        "",
        "This report is generated from structured Catch2 tags.",
        "",
        "## Summary",
        "",
        "| Tests | Classified | Exempt | Unclassified | Validation errors |",
        "| ---: | ---: | ---: | ---: | ---: |",
        f"| {len(tests)} | {len(tests) - exempt - unclassified} | {exempt} | {unclassified} | {len(errors)} |",
        "",
        "## Features",
        "",
    ]

    reported_dimensions = [dimension for dimension in dimensions if dimension != "feature"]
    headings = ["Feature", "Tests", *[dimension.replace("-", " ").title() for dimension in reported_dimensions]]
    lines.extend(
        [
            "| " + " | ".join(headings) + " |",
            "| " + " | ".join(["---", "---:", *(["---"] * len(reported_dimensions))]) + " |",
        ]
    )

    for feature in sorted(dimensions["feature"]):
        entries = feature_tests.get(feature, [])
        values = []
        for dimension in reported_dimensions:
            observed = sorted({value for entry in entries for value in entry.get(dimension, ())})
            values.append(", ".join(f"`{value}`" for value in observed) or "—")
        lines.append(f"| `{feature}` | {len(entries)} | " + " | ".join(values) + " |")

    uncovered_values = [
        (dimension, sorted(values - observed_values[dimension]))
        for dimension, values in dimensions.items()
        if values - observed_values[dimension]
    ]
    lines.extend(
        [
            "",
            "## Uncovered dimension values",
            "",
            "These values have no test tag anywhere in the suite.",
            "",
            "| Dimension | Values |",
            "| --- | --- |",
        ]
    )
    lines.extend(
        f"| {dimension} | " + ", ".join(f"`{value}`" for value in values) + " |"
        for dimension, values in uncovered_values
    )

    interaction_dimensions = (
        ("series", "scalar"),
        ("series", "declaration"),
        ("series", "binding"),
        ("series", "shape"),
        ("series", "transition"),
        ("schema", "representation"),
        ("representation", "binding"),
        ("binding", "shape"),
        ("activity", "validity"),
        ("guarantee", "series"),
        ("definition", "series"),
    )
    interactions = []
    for feature, entries in sorted(feature_tests.items()):
        for left, right in interaction_dimensions:
            counts = Counter(
                pair
                for entry in entries
                if left in entry and right in entry
                for pair in product(entry[left], entry[right])
            )
            if not counts:
                continue

            left_values = sorted({pair[0] for pair in counts})
            right_values = sorted({pair[1] for pair in counts})
            missing = [pair for pair in product(left_values, right_values) if pair not in counts]
            covered = ", ".join(f"`{a}` × `{b}` ({counts[a, b]})" for a, b in sorted(counts))
            missing_text = ", ".join(f"`{a}` × `{b}`" for a, b in missing) or "—"
            interactions.append((feature, f"{left} × {right}", covered, missing_text))

    lines.extend(
        [
            "",
            "## Candidate interaction gaps",
            "",
            "Missing combinations use only dimension values already exercised by the feature.",
            "Review whether a combination applies before adding a test.",
            "",
            "| Feature | Dimensions | Covered combinations | Missing combinations |",
            "| --- | --- | --- | --- |",
        ]
    )
    lines.extend(
        f"| `{feature}` | {pair} | {covered} | {missing} |"
        for feature, pair, covered, missing in interactions
    )

    if errors:
        lines.extend(["", "## Validation errors", ""])
        lines.extend(f"- {error}" for error in errors)

    return "\n".join(lines) + "\n"


def main() -> int:
    options = arguments()
    dimensions = read_dimensions(options.dimensions)
    tests = read_tests(options.test_executable)
    errors = validate_tests(tests, dimensions)
    report = markdown_report(tests, dimensions, errors)

    if options.output:
        options.output.parent.mkdir(parents=True, exist_ok=True)
        options.output.write_text(report)
    else:
        sys.stdout.write(report)

    return int(options.check and bool(errors))


if __name__ == "__main__":
    raise SystemExit(main())

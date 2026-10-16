#!/usr/bin/env python3
"""Run the portable high-count map workload on hgraph's Python runtime."""

from __future__ import annotations

import argparse
import json
import os
import platform
import resource
import subprocess
import sys
import sysconfig
import time
from pathlib import Path

import hgraph as hg
import _hgraph
from hgraph import TS, TSD, compute_node, graph, map_, sink_node

from benchmarks.high_count_map_workload import (
    Branch,
    EventChecksum,
    Lifecycle,
    build_workload,
    child_source_value,
    flat_source_value,
    initial_child_value,
    initial_flat_value,
    verify_workload_manifest,
)


CHILD_COUNT = 8
SOURCE_COMMIT = "de44e25b61dc57e217f9781164eee64b6b77e7cf"
WORKLOAD = None
CONTROL = None


def flat_events():
    profile = WORKLOAD.selected_profile
    yield hg.MIN_ST, {key: initial_flat_value(key) for key in range(profile.flat_count)}

    for cycle, plan in enumerate(WORKLOAD.cycles):
        delta = {mutation.key: mutation.value for mutation in plan.flat_updates}
        if plan.lifecycle == Lifecycle.REMOVE_FLAT_AND_PARENTS:
            delta.update({key: hg.REMOVE for key in plan.flat_lifecycle_keys})
        elif plan.lifecycle == Lifecycle.RESTORE_FLAT_AND_PARENTS:
            delta.update({key: flat_source_value(cycle, key) for key in plan.flat_lifecycle_keys})
        yield hg.MIN_ST + (cycle + 1) * hg.MIN_TD, delta


def parent_events():
    profile = WORKLOAD.selected_profile
    yield hg.MIN_ST, {
        parent_key: {
            child_key: initial_child_value(parent_key, child_key)
            for child_key in range(CHILD_COUNT)
        }
        for parent_key in range(profile.parent_count)
    }

    for cycle, plan in enumerate(WORKLOAD.cycles):
        delta = {}
        for update in plan.child_updates:
            delta.setdefault(update.parent_key, {})[update.child_key] = update.value

        if plan.lifecycle == Lifecycle.REMOVE_FLAT_AND_PARENTS:
            delta.update({parent_key: hg.REMOVE for parent_key in plan.parent_lifecycle_keys})
        elif plan.lifecycle == Lifecycle.RESTORE_FLAT_AND_PARENTS:
            for parent_key in plan.parent_lifecycle_keys:
                delta[parent_key] = {
                    child_key: child_source_value(cycle, parent_key, child_key)
                    for child_key in range(CHILD_COUNT)
                }
        elif plan.lifecycle == Lifecycle.REMOVE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                delta.setdefault(parent_key, {})[child_key] = hg.REMOVE
        elif plan.lifecycle == Lifecycle.RESTORE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                delta.setdefault(parent_key, {})[child_key] = child_source_value(
                    cycle, parent_key, child_key
                )

        yield hg.MIN_ST + (cycle + 1) * hg.MIN_TD, delta


def bias_events():
    yield hg.MIN_ST, 1
    for cycle, plan in enumerate(WORKLOAD.cycles):
        if plan.bias is not None:
            yield hg.MIN_ST + (cycle + 1) * hg.MIN_TD, plan.bias


@hg.generator
def flat_input() -> TSD[int, TS[int]]:
    yield from flat_events()


@hg.generator
def parent_input() -> TSD[int, TSD[int, TS[int]]]:
    yield from parent_events()


@hg.generator
def bias_input() -> TS[int]:
    yield from bias_events()


@compute_node
def add_values(lhs: TS[int], rhs: TS[int]) -> TS[int]:
    return lhs.value + rhs.value


@compute_node
def multiply(value: TS[int], factor: int) -> TS[int]:
    return value.value * factor


@compute_node
def add_constant(value: TS[int], addend: int) -> TS[int]:
    return value.value + addend


@graph
def stage_a(key: TS[int], value: TS[int], bias: TS[int]) -> TS[int]:
    a1 = add_values(value, bias)
    a2 = multiply(a1, 3)
    a3 = add_values(a2, key)
    a4 = multiply(a3, 5)
    return add_constant(a4, 7)


@graph
def stage_b(key: TS[int], value: TS[int], bias: TS[int]) -> TS[int]:
    b1 = add_values(value, bias)
    b2 = multiply(b1, 7)
    b3 = add_values(b2, key)
    b4 = multiply(b3, 3)
    return add_constant(b4, 11)


@sink_node
def observe_flat_collection(values: TSD[int, TS[int]]) -> None:
    for key in values.modified_keys():
        CONTROL.observe(Branch.FLAT, 0, key, True, values.value[key])
    for key in values.removed_keys():
        CONTROL.observe(Branch.FLAT, 0, key, False, 0)


@graph
def child_graph(
    child_key: TS[int],
    value: TS[int],
    parent_key: TS[int],
    seed: TS[int],
    bias: TS[int],
) -> TS[int]:
    c1 = add_values(value, seed)
    c2 = multiply(c1, 3)
    c3 = add_values(c2, child_key)
    c4 = add_values(c3, bias)
    return multiply(c4, 7)


@sink_node
def observe_parent_child_deltas(
    children: TSD[int, TS[int]], parent_key: TS[int]
) -> None:
    for child_key in children.modified_keys():
        CONTROL.observe(
            Branch.CHILD,
            parent_key.value,
            child_key,
            True,
            children.value[child_key],
        )
    for child_key in children.removed_keys():
        CONTROL.observe(Branch.CHILD, parent_key.value, child_key, False, 0)


@graph
def parent_graph(
    parent_key: TS[int], children: TSD[int, TS[int]], bias: TS[int]
) -> TSD[int, TS[int]]:
    p1 = add_values(parent_key, bias)
    seed = multiply(p1, 5)
    outputs = map_(
        child_graph,
        children,
        parent_key=parent_key,
        seed=seed,
        bias=bias,
        __key_arg__="child_key",
    )
    observe_parent_child_deltas(outputs, parent_key)
    return outputs


@sink_node
def observe_removed_parents(
    parents: TSD[int, TSD[int, TS[int]]]
) -> None:
    for parent_key in parents.removed_keys():
        for child_key in range(CHILD_COUNT):
            CONTROL.observe(Branch.CHILD, parent_key, child_key, False, 0)


@graph
def benchmark_graph() -> None:
    flat = flat_input()
    parents = parent_input()
    bias = bias_input()

    first_stage = map_(stage_a, flat, bias, __key_arg__="key")
    flat_output = map_(stage_b, first_stage, bias, __key_arg__="key")
    observe_flat_collection(flat_output)

    child_output = map_(parent_graph, parents, bias, __key_arg__="parent_key")
    observe_removed_parents(child_output)


def current_rss_bytes() -> int | None:
    try:
        with open("/proc/self/statm", encoding="ascii") as statm:
            resident_pages = int(statm.read().split()[1])
        return resident_pages * os.sysconf("SC_PAGE_SIZE")
    except (OSError, ValueError, IndexError):
        return None


def peak_rss_bytes() -> int | None:
    try:
        maximum = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    except (ValueError, OSError):
        return None
    return int(maximum if sys.platform == "darwin" else maximum * 1024)


def latency_class(plan) -> str:
    if plan.bias is not None:
        return "broadcast"
    return {
        Lifecycle.NONE: "ordinary",
        Lifecycle.REMOVE_FLAT_AND_PARENTS: "flat_parent_removal",
        Lifecycle.RESTORE_FLAT_AND_PARENTS: "flat_parent_restoration_cleanup",
        Lifecycle.REMOVE_CHILDREN: "child_removal",
        Lifecycle.RESTORE_CHILDREN: "child_restoration_cleanup",
    }[plan.lifecycle]


def summarize(samples: list[int]) -> dict[str, float | int]:
    if not samples:
        return {
            "count": 0,
            "total_ns": 0,
            "mean_ns": 0,
            "median_ns": 0,
            "p95_ns": 0,
            "p99_ns": 0,
            "max_ns": 0,
        }

    ordered = sorted(samples)

    def percentile(quantile: float) -> float:
        position = quantile * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)

    return {
        "count": len(ordered),
        "total_ns": sum(ordered),
        "mean_ns": sum(ordered) / len(ordered),
        "median_ns": percentile(0.50),
        "p95_ns": percentile(0.95),
        "p99_ns": percentile(0.99),
        "max_ns": ordered[-1],
    }


class RunControl:
    def __init__(self, workload):
        self.workload = workload
        self.measuring = False
        self.cycle = -1
        self.actual = EventChecksum()

    def observe(self, branch, parent_key, item_key, present, value):
        if self.measuring:
            self.actual.add(
                self.cycle, branch, parent_key, item_key, present, value
            )


class Measurements(hg.EvaluationLifeCycleObserver):
    def __init__(self, workload, control):
        self.workload = workload
        self.control = control
        self.root_evaluations = 0
        self.cycle_start_ns = None
        self.root_start_ns = None
        self.runtime_start_ns = None
        self.population_ns = None
        self.final_stop_ns = None
        self.baseline_rss_bytes = current_rss_bytes()
        self.after_wiring_rss_bytes = None
        self.empty_runtime_rss_bytes = None
        self.populated_rss_bytes = None
        self.after_stop_rss_bytes = None
        self.latencies = {
            "ordinary": [],
            "flat_parent_removal": [],
            "flat_parent_restoration_cleanup": [],
            "child_removal": [],
            "child_restoration_cleanup": [],
            "broadcast": [],
        }

    @staticmethod
    def is_root(graph_view):
        return graph_view.graph_id == ()

    def on_before_start_graph(self, graph_view):
        if self.is_root(graph_view):
            self.after_wiring_rss_bytes = current_rss_bytes()
            self.root_start_ns = time.perf_counter_ns()

    def on_after_start_graph(self, graph_view):
        if self.is_root(graph_view):
            self.runtime_start_ns = time.perf_counter_ns() - self.root_start_ns
            self.empty_runtime_rss_bytes = current_rss_bytes()

    def on_before_graph_evaluation(self, graph_view):
        if not self.is_root(graph_view):
            return
        if self.root_evaluations == 0:
            self.control.measuring = False
            self.cycle_start_ns = time.perf_counter_ns()
            return
        self.control.cycle = self.root_evaluations - 1
        self.control.measuring = True
        self.cycle_start_ns = time.perf_counter_ns()

    def on_after_graph_evaluation(self, graph_view):
        if not self.is_root(graph_view):
            return
        elapsed_ns = time.perf_counter_ns() - self.cycle_start_ns
        if self.root_evaluations == 0:
            self.population_ns = elapsed_ns
            self.populated_rss_bytes = current_rss_bytes()
            self.control.measuring = False
        else:
            cycle = self.root_evaluations - 1
            if cycle < len(self.workload.cycles):
                self.latencies[latency_class(self.workload.cycles[cycle])].append(
                    elapsed_ns
                )
        self.root_evaluations += 1

    def on_before_stop_graph(self, graph_view):
        if self.is_root(graph_view):
            self.root_start_ns = time.perf_counter_ns()

    def on_after_stop_graph(self, graph_view):
        if self.is_root(graph_view):
            self.final_stop_ns = time.perf_counter_ns() - self.root_start_ns
            self.after_stop_rss_bytes = current_rss_bytes()


def verify_native_header(path: Path, source_commit: str) -> str:
    header = path.read_text(encoding="utf-8")
    marker = 'git_commit_hash = "'
    start = header.find(marker)
    if start < 0:
        raise RuntimeError(f"native version header has no git commit hash: {path}")
    start += len(marker)
    end = header.find('"', start)
    native_commit = header[start:end]
    if native_commit != source_commit:
        raise RuntimeError(
            f"native extension source commit {native_commit} does not match "
            f"benchmark source commit {source_commit}"
        )
    build_dir = path.parents[3]
    expected_native_dir = build_dir / "python"
    if Path(_hgraph.__file__).resolve().parent != expected_native_dir.resolve():
        raise RuntimeError(
            f"loaded native extension is not from the checked build: {_hgraph.__file__}"
        )
    expected_source = build_dir.parent / "src" / "python" / "hgraph" / "__init__.py"
    if Path(hg.__file__).resolve() != expected_source.resolve():
        raise RuntimeError(
            f"loaded hgraph package is not from the checked source: {hg.__file__}"
        )
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise RuntimeError(f"hgraph native build is not Release: {build_dir}")
    return header.split('version_string = "', 1)[1].split('"', 1)[0]


def run(profile_name: str, cycle_count: int, version_header: Path) -> dict:
    global CONTROL, WORKLOAD

    source_version = verify_native_header(version_header, SOURCE_COMMIT)
    workload_start = time.perf_counter_ns()
    verify_workload_manifest()
    WORKLOAD = build_workload(profile_name, cycle_count)
    workload_generation_ns = time.perf_counter_ns() - workload_start

    CONTROL = RunControl(WORKLOAD)
    measurements = Measurements(WORKLOAD, CONTROL)
    config = hg.GraphConfiguration(
        start_time=hg.MIN_ST,
        life_cycle_observers=(measurements,),
        default_log_level=50,
    )
    run_start = time.perf_counter_ns()
    hg.evaluate_graph(benchmark_graph, config)
    wall_run_ns = time.perf_counter_ns() - run_start

    measured_sample_count = sum(
        len(samples) for samples in measurements.latencies.values()
    )
    if measurements.root_evaluations != cycle_count + 1:
        raise RuntimeError(
            f"expected one population evaluation and {cycle_count} measured cycles, "
            f"got {measurements.root_evaluations} root evaluations"
        )
    if measured_sample_count != cycle_count:
        raise RuntimeError(
            f"expected {cycle_count} measured cycle samples, got {measured_sample_count}"
        )

    expected = WORKLOAD.expected
    valid = CONTROL.actual == expected
    if not valid:
        raise RuntimeError(
            "output checksum mismatch: "
            f"actual=({CONTROL.actual.count}, {CONTROL.actual.sum}, {CONTROL.actual.xor_value}), "
            f"expected=({expected.count}, {expected.sum}, {expected.xor_value})"
        )

    latency = {name: summarize(samples) for name, samples in measurements.latencies.items()}
    measured_cycles_ns = sum(item["total_ns"] for item in latency.values())
    profile = WORKLOAD.selected_profile
    rss_delta = None
    bytes_per_graph = None
    if measurements.populated_rss_bytes is not None and measurements.empty_runtime_rss_bytes is not None:
        rss_delta = measurements.populated_rss_bytes - measurements.empty_runtime_rss_bytes
        bytes_per_graph = rss_delta / profile.active_inner_graphs

    try:
        affinity = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        affinity = None
    adapter_commit = subprocess.run(
        ["git", "-C", str(Path(__file__).resolve().parents[2]), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()

    return {
        "schema_version": 1,
        "benchmark": "portable_high_count_map",
        "engine": "hgraph-current",
        "runtime": "python",
        "hgraph_version": source_version,
        "source_commit": SOURCE_COMMIT,
        "benchmark_adapter_commit": adapter_commit,
        "profile": profile.name,
        "value_type": "int64_nonnegative",
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpu_count": os.cpu_count(),
            "cpu_affinity": affinity,
            "python": platform.python_version(),
            "compiler": sysconfig.get_config_var("CXX"),
            "build_type": "Release",
            "python_module": str(Path(hg.__file__).resolve()),
            "native_version_header": str(version_header.resolve()),
        },
        "notes": [
            "run_graph combines graph wiring, runtime construction, population, measured cycles, and shutdown; "
            "the Python runner does not expose those phases separately.",
            "Outer-cycle latency includes source delivery through completion of nested output sinks.",
            "RSS after wiring is sampled immediately before root start; empty-runtime RSS is sampled after "
            "root start and before population.",
        ],
        "topology": {
            "flat_items": profile.flat_count,
            "parents": profile.parent_count,
            "children_per_parent": CHILD_COUNT,
            "active_inner_graphs": profile.active_inner_graphs,
        },
        "workload": {
            "cycles": cycle_count,
            "flat_updates_per_cycle": profile.flat_updates,
            "child_updates_per_cycle": profile.child_updates,
            "flat_lifecycle_count": profile.flat_lifecycle_count,
            "parent_lifecycle_count": profile.parent_lifecycle_count,
            "child_lifecycle_count": profile.child_lifecycle_count,
            "lifecycle_period": 100,
            "broadcast_period": 1000,
        },
        "timing": {
            "workload_generation_ns": workload_generation_ns,
            "wiring_ns": None,
            "runtime_factory_ns": None,
            "runtime_start_ns": measurements.runtime_start_ns,
            "population_ns": measurements.population_ns,
            "measured_cycles_ns": measured_cycles_ns,
            "wall_run_ns": wall_run_ns,
            "final_stop_ns": measurements.final_stop_ns,
        },
        "throughput": {
            "cycles_per_second": cycle_count * 1e9 / measured_cycles_ns,
            "output_events_per_second": CONTROL.actual.count * 1e9 / measured_cycles_ns,
        },
        "latency": latency,
        "output": {
            "actual": {
                "count": CONTROL.actual.count,
                "sum": CONTROL.actual.sum,
                "xor": CONTROL.actual.xor_value,
            },
            "expected": {
                "count": expected.count,
                "sum": expected.sum,
                "xor": expected.xor_value,
            },
            "valid": valid,
        },
        "memory": {
            "baseline_rss_bytes": measurements.baseline_rss_bytes,
            "after_wiring_rss_bytes": measurements.after_wiring_rss_bytes,
            "empty_runtime_rss_bytes": measurements.empty_runtime_rss_bytes,
            "populated_rss_bytes": measurements.populated_rss_bytes,
            "peak_rss_bytes": peak_rss_bytes(),
            "after_stop_rss_bytes": measurements.after_stop_rss_bytes,
            "populated_rss_delta_bytes": rss_delta,
            "bytes_per_active_inner_graph": bytes_per_graph,
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("smoke", "25k", "100k", "250k"), default="smoke")
    parser.add_argument("--cycles", type=int, default=120)
    parser.add_argument("--native-version-header", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    if arguments.cycles < 1:
        parser.error("--cycles must be positive")

    result = run(arguments.profile, arguments.cycles, arguments.native_version_header)
    output = json.dumps(result, indent=2) + "\n"
    if arguments.output:
        arguments.output.write_text(output, encoding="utf-8")
    else:
        sys.stdout.write(output)


if __name__ == "__main__":
    main()

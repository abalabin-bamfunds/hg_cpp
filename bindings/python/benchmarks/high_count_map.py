"""Run the portable high-count map workload through the BG Python API.

Run from the repository root with:

    PYTHONPATH=build/python313/bindings/python python bindings/python/benchmarks/high_count_map.py \
      --profile smoke --cycles 120

The public Python runner owns wiring, compilation, runtime creation, evaluation,
and shutdown as one operation. The report leaves phase timings null when the
runner does not expose a boundary. The graph uses one Python callback per
arithmetic operation, matching the Python adapters for other engines.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
from typing import Any

try:
    import resource
except ImportError:
    resource = None


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

import bg
from benchmarks.high_count_map_workload import (
    CHILD_COUNT,
    PROFILES,
    Branch,
    EventChecksum,
    Lifecycle,
    Workload,
    build_workload,
    child_source_value,
    flat_source_value,
    initial_child_value,
    initial_flat_value,
    verify_workload_manifest,
)


class RunObserver:
    def __init__(self, workload: Workload) -> None:
        self.actual = EventChecksum()
        self.active_children: dict[int, set[int]] = {}
        self.workload = workload
        self.cycle_starts = [0] * len(workload.cycles)
        self.cycle_finishes = [0] * len(workload.cycles)
        self.cycle_event_counts = [0] * len(workload.cycles)
        self.cycle_observers: list[set[str]] = [set() for _ in workload.cycles]
        self.population_started_ns: int | None = None
        self.population_finished_ns: int | None = None
        self.population_observers: set[str] = set()
        self.populated_rss_bytes: int | None = None
        self.before_stop_rss_bytes: int | None = None

    def start(self, cycle: int) -> None:
        now = time.perf_counter_ns()
        if cycle == 0:
            self.population_started_ns = now
        else:
            self.cycle_starts[cycle - 1] = now

    def complete(self, cycle: int, branch: str) -> None:
        if cycle < 0:
            self.population_observers.add(branch)
            if len(self.population_observers) == 2:
                self.population_finished_ns = time.perf_counter_ns()
                self.populated_rss_bytes = current_rss_bytes()
            return

        observers = self.cycle_observers[cycle]
        observers.add(branch)
        if len(observers) == 2:
            self.cycle_finishes[cycle] = time.perf_counter_ns()
            if cycle + 1 == len(self.workload.cycles):
                self.before_stop_rss_bytes = current_rss_bytes()


def expected_cycle_event_count(workload: Workload, cycle: int) -> int:
    profile = workload.selected_profile
    plan = workload.cycles[cycle]
    updates = len(plan.flat_updates) + len(plan.child_updates)
    if plan.bias is not None:
        return profile.flat_count + profile.parent_count * CHILD_COUNT
    if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
        return updates + len(plan.flat_lifecycle_keys) + len(plan.parent_lifecycle_keys) * CHILD_COUNT
    if plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
        return updates + len(plan.flat_lifecycle_keys) + len(plan.parent_lifecycle_keys) * CHILD_COUNT
    if plan.lifecycle in (Lifecycle.REMOVE_CHILDREN, Lifecycle.RESTORE_CHILDREN):
        return updates + len(plan.child_lifecycle_keys)
    return updates


def latency_class(plan: Any) -> str:
    if plan.bias is not None:
        return "broadcast"
    if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
        return "flat_parent_removal"
    if plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
        return "flat_parent_restoration_cleanup"
    if plan.lifecycle is Lifecycle.REMOVE_CHILDREN:
        return "child_removal"
    if plan.lifecycle is Lifecycle.RESTORE_CHILDREN:
        return "child_restoration_cleanup"
    return "ordinary"


def summarize_latencies(samples: list[int]) -> dict[str, float | int]:
    if not samples:
        return {"count": 0, "total_ns": 0.0, "mean_ns": 0.0, "median_ns": 0.0,
                "p95_ns": 0.0, "p99_ns": 0.0, "max_ns": 0.0}
    ordered = sorted(samples)

    def percentile(quantile: float) -> float:
        position = quantile * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)

    total = sum(ordered)
    return {
        "count": len(ordered),
        "total_ns": float(total),
        "mean_ns": total / len(ordered),
        "median_ns": percentile(0.5),
        "p95_ns": percentile(0.95),
        "p99_ns": percentile(0.99),
        "max_ns": float(ordered[-1]),
    }


def current_rss_bytes() -> int | None:
    if sys.platform.startswith("linux"):
        try:
            with open("/proc/self/statm", encoding="ascii") as statm:
                resident_pages = int(statm.read().split()[1])
            return resident_pages * os.sysconf("SC_PAGE_SIZE")
        except (OSError, ValueError, IndexError):
            return None
    return None


def peak_rss_bytes() -> int | None:
    if resource is None:
        return None
    try:
        peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    except (AttributeError, OSError):
        return None
    return peak if sys.platform == "darwin" else peak * 1024


def _mapping(value: Any) -> dict[Any, Any]:
    return dict(value)


def git_commit() -> str | None:
    try:
        return subprocess.check_output(
            ["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "HEAD"], text=True
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def make_graph(workload: Workload, observer: RunObserver, arithmetic: str):
    @bg.compute_node
    def flat_source(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        cycle = trigger.value
        observer.start(cycle)
        if cycle == 0:
            return {key: initial_flat_value(key) for key in range(workload.selected_profile.flat_count)}

        measured_cycle = cycle - 1
        plan = workload.cycles[measured_cycle]
        changes = {update.key: update.value for update in plan.flat_updates}
        if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
            changes.update({key: bg.REMOVE for key in plan.flat_lifecycle_keys})
        elif plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
            changes.update({key: flat_source_value(measured_cycle, key) for key in plan.flat_lifecycle_keys})
        return changes or None

    @bg.compute_node
    def parent_source(
        trigger: bg.TS[int],
    ) -> bg.TSD[int, bg.TSD[int, bg.TS[int]]]:
        cycle = trigger.value
        if cycle == 0:
            return {
                parent_key: {
                    child_key: initial_child_value(parent_key, child_key)
                    for child_key in range(CHILD_COUNT)
                }
                for parent_key in range(workload.selected_profile.parent_count)
            }

        measured_cycle = cycle - 1
        plan = workload.cycles[measured_cycle]
        changes: dict[int, Any] = {}
        for update in plan.child_updates:
            changes.setdefault(update.parent_key, {})[update.child_key] = update.value

        if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
            changes.update({key: bg.REMOVE for key in plan.parent_lifecycle_keys})
        elif plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
            changes.update(
                {
                    parent_key: {
                        child_key: child_source_value(measured_cycle, parent_key, child_key)
                        for child_key in range(CHILD_COUNT)
                    }
                    for parent_key in plan.parent_lifecycle_keys
                }
            )
        elif plan.lifecycle is Lifecycle.REMOVE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                changes.setdefault(parent_key, {})[child_key] = bg.REMOVE
        elif plan.lifecycle is Lifecycle.RESTORE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                changes.setdefault(parent_key, {})[child_key] = child_source_value(
                    measured_cycle, parent_key, child_key
                )
        return changes or None

    @bg.compute_node
    def bias_source(trigger: bg.TS[int]) -> bg.TS[int]:
        if trigger.value == 0:
            return 1
        return workload.cycles[trigger.value - 1].bias

    if arithmetic == "library":
        add_values = bg.operators.add
        multiply = bg.operators.mul
        add_constant = bg.operators.add
    else:
        @bg.compute_node
        def add_values(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
            return lhs.value + rhs.value

        @bg.compute_node
        def multiply(value: bg.TS[int], factor: int) -> bg.TS[int]:
            return value.value * factor

        @bg.compute_node
        def add_constant(value: bg.TS[int], addend: int) -> bg.TS[int]:
            return value.value + addend

    @bg.graph
    def stage_a(key: bg.TS[int], value: bg.TS[int], bias: bg.TS[int]) -> bg.TS[int]:
        a1 = add_values(value, bias)
        a2 = multiply(a1, 3)
        a3 = add_values(a2, key)
        a4 = multiply(a3, 5)
        return add_constant(a4, 7)

    @bg.graph
    def stage_b(key: bg.TS[int], value: bg.TS[int], bias: bg.TS[int]) -> bg.TS[int]:
        b1 = add_values(value, bias)
        b2 = multiply(b1, 7)
        b3 = add_values(b2, key)
        b4 = multiply(b3, 3)
        return add_constant(b4, 11)

    @bg.graph
    def child_branch(
        child_key: bg.TS[int],
        value: bg.TS[int],
        parent_key: bg.TS[int],
        seed: bg.TS[int],
        bias: bg.TS[int],
    ) -> bg.TS[int]:
        c1 = add_values(value, seed)
        c2 = multiply(c1, 3)
        c3 = add_values(c2, child_key)
        c4 = add_values(c3, bias)
        return multiply(c4, 7)

    @bg.graph
    def parent_branch(
        parent_key: bg.TS[int],
        children: bg.TSD[int, bg.TS[int]],
        bias: bg.TS[int],
    ) -> bg.TSD[int, bg.REF[bg.TS[int]]]:
        p1 = add_values(bias, parent_key)
        seed = multiply(p1, 5)
        return bg.map(child_branch, children, parent_key, seed, bias, __key_arg__="child_key")

    @bg.compute_node
    def observe_flat(trigger: bg.TS[int], values: bg.TSD[int, bg.TS[int]]) -> None:
        delta = values.delta_value
        cycle = trigger.value - 1
        if delta is not None and cycle >= 0:
            for key, value in _mapping(delta).items():
                if value is bg.REMOVE:
                    observer.actual.add(cycle, Branch.FLAT, 0, key, False, 0)
                else:
                    observer.actual.add(cycle, Branch.FLAT, 0, key, True, value)
                observer.cycle_event_counts[cycle] += 1
        observer.complete(cycle, "flat")

    @bg.compute_node
    def observe_children(
        trigger: bg.TS[int],
        values: bg.TSD[int, bg.TSD[int, bg.TS[int]]],
    ) -> None:
        delta = values.delta_value
        cycle = trigger.value - 1
        if delta is not None:
            for parent_key, child_delta in _mapping(delta).items():
                if cycle < 0:
                    observer.active_children[parent_key] = set(_mapping(child_delta))
                    continue
                if child_delta is bg.REMOVE:
                    for child_key in observer.active_children.pop(parent_key, set()):
                        observer.actual.add(cycle, Branch.CHILD, parent_key, child_key, False, 0)
                        observer.cycle_event_counts[cycle] += 1
                    continue

                active = observer.active_children.setdefault(parent_key, set())
                for child_key, value in _mapping(child_delta).items():
                    if value is bg.REMOVE:
                        observer.actual.add(cycle, Branch.CHILD, parent_key, child_key, False, 0)
                        active.discard(child_key)
                    else:
                        observer.actual.add(cycle, Branch.CHILD, parent_key, child_key, True, value)
                        active.add(child_key)
                    observer.cycle_event_counts[cycle] += 1
        observer.complete(cycle, "child")

    @bg.graph
    def benchmark(trigger: bg.TS[int]) -> bg.TS[int]:
        flat_values = flat_source(trigger)
        parents = parent_source(trigger)
        bias = bias_source(trigger)
        stage_a_values = bg.map(stage_a, flat_values, bias, __key_arg__="key")
        flat_output = bg.map(stage_b, stage_a_values, bias, __key_arg__="key")
        child_output = bg.map(parent_branch, parents, bias, __key_arg__="parent_key")
        observe_flat(trigger, flat_output)
        observe_children(trigger, child_output)
        return trigger

    return benchmark


def run_benchmark(profile_name: str, cycle_count: int, arithmetic: str = "callbacks") -> dict[str, Any]:
    started = time.perf_counter_ns()
    workload = build_workload(profile_name, cycle_count)
    workload_generation_ns = time.perf_counter_ns() - started
    observer = RunObserver(workload)
    definition = make_graph(workload, observer, arithmetic)
    triggers = list(range(cycle_count + 1))
    before_wiring_rss = current_rss_bytes()

    runner_started = time.perf_counter_ns()
    bg.test_graph(definition, triggers)
    runner_ns = time.perf_counter_ns() - runner_started
    after_stop_rss = current_rss_bytes()
    process_peak_rss = peak_rss_bytes()

    expected_event_counts = [expected_cycle_event_count(workload, cycle) for cycle in range(cycle_count)]
    for cycle, expected_count in enumerate(expected_event_counts):
        if observer.cycle_event_counts[cycle] != expected_count:
            raise RuntimeError(
                f"cycle {cycle} emitted {observer.cycle_event_counts[cycle]} events; expected {expected_count}"
            )
        if not observer.cycle_starts[cycle] or not observer.cycle_finishes[cycle]:
            raise RuntimeError(f"cycle {cycle} did not reach both output observers")

    if observer.actual != workload.expected:
        raise RuntimeError(
            "output checksum mismatch: "
            f"actual={observer.actual!r}, expected={workload.expected!r}"
        )

    measured_cycle_ns = [
        observer.cycle_finishes[cycle] - observer.cycle_starts[cycle]
        for cycle in range(cycle_count)
    ]
    latency: dict[str, list[int]] = {
        name: []
        for name in (
            "ordinary",
            "flat_parent_removal",
            "flat_parent_restoration_cleanup",
            "child_removal",
            "child_restoration_cleanup",
            "broadcast",
        )
    }
    for cycle, duration in enumerate(measured_cycle_ns):
        latency[latency_class(workload.cycles[cycle])].append(duration)
    measured_cycles_total_ns = sum(measured_cycle_ns)
    source_commit = os.environ.get("BG_SOURCE_COMMIT") or git_commit()
    adapter_commit = os.environ.get("BG_BENCHMARK_ADAPTER_COMMIT") or git_commit()
    try:
        cpu_affinity = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cpu_affinity = None

    return {
        "schema_version": 1,
        "benchmark": "portable_high_count_map",
        "engine": "bg",
        "runtime": "library-operators" if arithmetic == "library" else "python-callbacks",
        "source_commit": source_commit,
        "benchmark_adapter_commit": adapter_commit,
        "profile": profile_name,
        "value_type": "int64_nonnegative",
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor() or None,
            "logical_cpu_count": os.cpu_count(),
            "cpu_affinity": cpu_affinity,
            "python": sys.version.split()[0],
            "python_implementation": platform.python_implementation(),
            "build_type": "Release",
        },
        "topology": {
            "flat_items": workload.selected_profile.flat_count,
            "parents": workload.selected_profile.parent_count,
            "children_per_parent": CHILD_COUNT,
            "active_inner_graphs": workload.selected_profile.active_inner_graphs,
        },
        "workload": {
            "cycles": cycle_count,
            "flat_updates_per_cycle": workload.selected_profile.flat_updates,
            "child_updates_per_cycle": workload.selected_profile.child_updates,
            "flat_lifecycle_count": workload.selected_profile.flat_lifecycle_count,
            "parent_lifecycle_count": workload.selected_profile.parent_lifecycle_count,
            "child_lifecycle_count": workload.selected_profile.child_lifecycle_count,
            "lifecycle_period": 100,
            "broadcast_period": 1_000,
        },
        "timing": {
            "workload_generation_ns": workload_generation_ns,
            "wiring_ns": None,
            "runtime_factory_ns": None,
            "runtime_start_ns": None,
            "population_ns": (
                observer.population_finished_ns - observer.population_started_ns
                if observer.population_started_ns is not None and observer.population_finished_ns is not None
                else None
            ),
            "measured_cycles_ns": measured_cycles_total_ns,
            "wall_run_ns": runner_ns,
            "final_stop_ns": None,
        },
        "throughput": {
            "cycles_per_second": cycle_count * 1.0e9 / measured_cycles_total_ns,
            "output_events_per_second": observer.actual.count * 1.0e9 / measured_cycles_total_ns,
        },
        "latency": {name: summarize_latencies(samples) for name, samples in latency.items()},
        "memory": {
            "baseline_rss_bytes": before_wiring_rss,
            "after_wiring_rss_bytes": None,
            "empty_runtime_rss_bytes": None,
            "populated_rss_bytes": observer.populated_rss_bytes,
            "before_stop_rss_bytes": observer.before_stop_rss_bytes,
            "graph_rss_delta_bytes": (
                observer.before_stop_rss_bytes - before_wiring_rss
                if observer.before_stop_rss_bytes is not None and before_wiring_rss is not None
                else None
            ),
            "peak_rss_bytes": max(
                value for value in (before_wiring_rss, observer.populated_rss_bytes, process_peak_rss, after_stop_rss)
                if value is not None
            ),
            "after_stop_rss_bytes": after_stop_rss,
            "populated_rss_delta_bytes": None,
            "bytes_per_active_inner_graph": None,
        },
        "output": {
            "actual": {
                "count": observer.actual.count,
                "sum": observer.actual.sum,
                "xor": observer.actual.xor_value,
            },
            "expected": {
                "count": workload.expected.count,
                "sum": workload.expected.sum,
                "xor": workload.expected.xor_value,
            },
            "valid": True,
        },
        "notes": [
            "bg.test_graph owns wiring, compilation, runtime creation, evaluation, and stop as one call.",
            "Per-cycle timing starts at flat_source and ends after both output observers; it excludes test_graph input dispatch.",
            "Per-cycle timing includes source callbacks, graph evaluation, and checksum observers.",
        ],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="smoke")
    parser.add_argument("--cycles", type=int, default=120)
    parser.add_argument("--arithmetic", choices=("callbacks", "library"), default="callbacks")
    parser.add_argument("--verify-workload", action="store_true")
    arguments = parser.parse_args()

    if arguments.verify_workload:
        verify_workload_manifest()
        print("workload manifest verified")
        return
    if arguments.cycles <= 0:
        parser.error("--cycles must be positive")

    print(json.dumps(run_benchmark(arguments.profile, arguments.cycles, arguments.arithmetic), indent=2))


if __name__ == "__main__":
    main()

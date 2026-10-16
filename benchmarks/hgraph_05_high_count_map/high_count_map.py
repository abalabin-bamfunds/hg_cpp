#!/usr/bin/env python3
"""Run the portable high-count map workload on the hgraph 0.5 Python runtime."""

import argparse
import json
import logging
import os
import platform
import resource
import statistics
import subprocess
import sys
import time
from collections import defaultdict
from datetime import timedelta
from pathlib import Path
from typing import Any

REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
if str(REPOSITORY_ROOT) not in sys.path:
    sys.path.insert(0, str(REPOSITORY_ROOT))

from benchmarks.high_count_map_workload import (  # noqa: E402
    PROFILES,
    EventChecksum,
    build_workload,
    child_source_value,
    flat_source_value,
    initial_child_value,
    initial_flat_value,
    verify_workload_manifest,
)

HGRAPH_05_COMMIT = "c34dba1db8a1c873ee303796db4c8fc48c8cc5be"
HGRAPH_05_VERSION = "0.5.42"
CHILD_COUNT = 8


class BenchmarkControl:
    def __init__(self, workload):
        self.workload = workload
        self.source_step = 0
        self.populating = False
        self.population_start_ns = None
        self.population_ns = None
        self.population_expected_events = (
            workload.selected_profile.flat_count + workload.selected_profile.parent_count * CHILD_COUNT
        )
        self.population_event_count = 0
        self.baseline_rss_bytes = None
        self.empty_runtime_rss_bytes = None
        self.after_wiring_rss_bytes = None
        self.populated_rss_bytes = None
        self.after_stop_rss_bytes = None
        self.actual = EventChecksum()
        self.actual_cycle_event_count = 0
        self.expected_cycle_event_count = 0
        self.cycle = -1
        self.cycle_start_ns = None
        self.cycle_latencies_ns = defaultdict(list)
        self.measured_start_ns = None
        self.measured_end_ns = None
        self.observed_child_values: dict[int, dict[int, int]] = {}
        self.final_stop_start_ns = None
        self.final_stop_ns = None

    def begin_population(self):
        self.populating = True
        self.population_start_ns = time.perf_counter_ns()

    def begin_cycle(self, cycle: int):
        self.cycle = cycle
        self.actual_cycle_event_count = 0
        self.expected_cycle_event_count = expected_event_count(self.workload, cycle)
        self.cycle_start_ns = time.perf_counter_ns()
        if self.measured_start_ns is None:
            self.measured_start_ns = self.cycle_start_ns

    def begin_final_stop(self):
        if self.measured_end_ns is None:
            self.measured_end_ns = time.perf_counter_ns()
        self.final_stop_start_ns = time.perf_counter_ns()

    def observe(self, branch: int, parent_key: int, item_key: int, present: bool, value: int):
        if self.populating:
            self.population_event_count += 1
            if self.population_event_count == self.population_expected_events:
                self.population_ns = time.perf_counter_ns() - self.population_start_ns
                self.populated_rss_bytes = current_rss_bytes()
                self.populating = False
            return

        if self.cycle_start_ns is None:
            return

        self.actual.add(self.cycle, branch, parent_key, item_key, present, value)
        self.actual_cycle_event_count += 1
        if self.actual_cycle_event_count == self.expected_cycle_event_count:
            elapsed_ns = time.perf_counter_ns() - self.cycle_start_ns
            self.cycle_latencies_ns[cycle_class(self.workload.cycles[self.cycle])].append(elapsed_ns)
            self.cycle_start_ns = None
            if self.cycle + 1 == len(self.workload.cycles):
                self.measured_end_ns = time.perf_counter_ns()


def expected_event_count(workload, cycle: int) -> int:
    plan = workload.cycles[cycle]
    lifecycle = lifecycle_name(plan)
    count = len(plan.flat_updates) + len(plan.child_updates)

    if lifecycle == "remove_flat_and_parents":
        count += len(plan.flat_lifecycle_keys) + len(plan.parent_lifecycle_keys) * CHILD_COUNT
    elif lifecycle == "restore_flat_and_parents":
        count += len(plan.flat_lifecycle_keys) + len(plan.parent_lifecycle_keys) * CHILD_COUNT
    elif lifecycle in {"remove_children", "restore_children"}:
        count += len(plan.child_lifecycle_keys)

    if plan.bias is not None:
        count = workload.selected_profile.flat_count + workload.selected_profile.parent_count * CHILD_COUNT
    return count


def lifecycle_name(plan) -> str:
    lifecycle = plan.lifecycle
    return lifecycle.value if hasattr(lifecycle, "value") else str(lifecycle)


def cycle_class(plan) -> str:
    if plan.bias is not None:
        return "broadcast"
    return {
        "none": "ordinary",
        "remove_flat_and_parents": "flat_parent_removal",
        "restore_flat_and_parents": "flat_parent_restoration_cleanup",
        "remove_children": "child_removal",
        "restore_children": "child_restoration_cleanup",
    }.get(lifecycle_name(plan), "broadcast")


def current_rss_bytes() -> int:
    try:
        import psutil

        return psutil.Process().memory_info().rss
    except ImportError:
        return 0


def peak_rss_bytes() -> int:
    value = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return int(value if sys.platform == "darwin" else value * 1024)


def statistics_for(values: list[int]) -> dict[str, int | float | None]:
    if not values:
        return {"count": 0, "total_ns": 0, "mean_ns": None, "median_ns": None,
                "p95_ns": None, "p99_ns": None, "max_ns": None}

    ordered = sorted(values)
    def percentile(quantile: float) -> float:
        position = quantile * (len(ordered) - 1)
        lower = int(position)
        upper = min(lower + 1, len(ordered) - 1)
        return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)

    return {
        "count": len(values),
        "total_ns": sum(values),
        "mean_ns": statistics.fmean(values),
        "median_ns": percentile(0.50),
        "p95_ns": percentile(0.95),
        "p99_ns": percentile(0.99),
        "max_ns": max(values),
    }


def git_commit() -> str | None:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=REPOSITORY_ROOT, text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=PROFILES, default="smoke")
    parser.add_argument("--cycles", type=int, default=120)
    parser.add_argument("--verify-workload", action="store_true")
    parser.add_argument("--source-commit", default=HGRAPH_05_COMMIT)
    args = parser.parse_args()

    if args.verify_workload:
        verify_workload_manifest()
        print(json.dumps({"workload_manifest": "valid"}, sort_keys=True))
        return 0

    if args.cycles < 1:
        parser.error("--cycles must be positive")

    return run_benchmark(args.profile, args.cycles, args.source_commit)


def run_benchmark(profile_name: str, cycles: int, source_commit: str) -> int:
    import hgraph
    from hgraph import EvaluationLifeCycleObserver, EvaluationMode, GraphConfiguration
    from hgraph import SIGNAL, TSB, TSB_OUT, TS, TSD, TimeSeriesSchema
    from hgraph import compute_node, graph, map_, schedule, sink_node
    from hgraph import wire_graph, evaluate_graph
    from hgraph._wiring._wiring_node_instance import WiringNodeInstanceContext

    workload_start_ns = time.perf_counter_ns()
    workload = build_workload(profile_name, cycles)
    workload_generation_ns = time.perf_counter_ns() - workload_start_ns
    control = BenchmarkControl(workload)
    control.baseline_rss_bytes = current_rss_bytes()

    class Inputs(TimeSeriesSchema):
        flat: TSD[int, TS[int]]
        parents: TSD[int, TSD[int, TS[int]]]
        bias: TS[int]

    globals().update(
        SIGNAL=SIGNAL,
        TSB=TSB,
        TSB_OUT=TSB_OUT,
        TS=TS,
        TSD=TSD,
        Inputs=Inputs,
    )

    @compute_node
    def source(tick: SIGNAL, _output: TSB_OUT[Inputs] = None) -> TSB[Inputs]:
        if control.source_step == 0:
            control.begin_population()
            for key in range(workload.selected_profile.flat_count):
                _output.flat.get_or_create(key).value = initial_flat_value(key)
            for parent_key in range(workload.selected_profile.parent_count):
                children = _output.parents.get_or_create(parent_key)
                for child_key in range(CHILD_COUNT):
                    children.get_or_create(child_key).value = initial_child_value(parent_key, child_key)
            _output.bias.value = 1
        else:
            cycle = control.source_step - 1
            control.begin_cycle(cycle)
            plan = workload.cycles[cycle]
            for update in plan.flat_updates:
                _output.flat[update.key].value = update.value
            for update in plan.child_updates:
                _output.parents[update.parent_key][update.child_key].value = update.value

            lifecycle = lifecycle_name(plan)
            if lifecycle == "remove_flat_and_parents":
                for key in plan.flat_lifecycle_keys:
                    del _output.flat[key]
                for parent_key in plan.parent_lifecycle_keys:
                    del _output.parents[parent_key]
            elif lifecycle == "restore_flat_and_parents":
                for key in plan.flat_lifecycle_keys:
                    _output.flat.get_or_create(key).value = flat_source_value(cycle, key)
                for parent_key in plan.parent_lifecycle_keys:
                    children = _output.parents.get_or_create(parent_key)
                    for child_key in range(CHILD_COUNT):
                        children.get_or_create(child_key).value = child_source_value(
                            cycle, parent_key, child_key
                        )
            elif lifecycle == "remove_children":
                for parent_key, child_key in plan.child_lifecycle_keys:
                    del _output.parents[parent_key][child_key]
            elif lifecycle == "restore_children":
                for parent_key, child_key in plan.child_lifecycle_keys:
                    _output.parents[parent_key].get_or_create(child_key).value = child_source_value(
                        cycle, parent_key, child_key
                    )

            if plan.bias is not None:
                _output.bias.value = plan.bias

        control.source_step += 1

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

    @graph
    def parent_graph(parent_key: TS[int], children: TSD[int, TS[int]], bias: TS[int]) -> TSD[int, TS[int]]:
        p1 = add_values(bias, parent_key)
        seed = multiply(p1, 5)
        return map_(
            child_graph,
            value=children,
            parent_key=parent_key,
            seed=seed,
            bias=bias,
            __key_arg__="child_key",
        )

    @sink_node
    def observe_outputs(flat: TSD[int, TS[int]], children: TSD[int, TSD[int, TS[int]]]):
        for key in set(flat.added_keys()) | set(flat.modified_keys()):
            value = int(flat[key].value)
            control.observe(1, 0, int(key), True, value)
        for key in flat.removed_keys():
            control.observe(1, 0, int(key), False, 0)

        for parent_key in children.removed_keys():
            for child_key in control.observed_child_values.pop(int(parent_key), {}):
                control.observe(2, int(parent_key), child_key, False, 0)

        active_parents = set(children.added_keys()) | set(children.modified_keys())
        for parent_key in active_parents:
            child_values = children[parent_key]
            if parent_key in children.added_keys():
                active_children = set(child_values.keys())
            else:
                active_children = set(child_values.added_keys()) | set(child_values.modified_keys())
            for child_key in active_children:
                value = int(child_values[child_key].value)
                control.observe(2, int(parent_key), int(child_key), True, value)
                control.observed_child_values.setdefault(int(parent_key), {})[int(child_key)] = value
            for child_key in child_values.removed_keys():
                control.observe(2, int(parent_key), int(child_key), False, 0)
                control.observed_child_values.get(int(parent_key), {}).pop(int(child_key), None)

    @graph
    def benchmark_graph(cycle_count: int):
        tick = schedule(timedelta(microseconds=1), initial_delay=False, max_ticks=cycle_count + 1)
        inputs = source(tick)
        first = map_(
            stage_a,
            value=inputs.flat,
            bias=inputs.bias,
            __key_arg__="key",
        )
        flat = map_(stage_b, value=first, bias=inputs.bias, __key_arg__="key")
        children = map_(
            parent_graph,
            children=inputs.parents,
            bias=inputs.bias,
            __key_arg__="parent_key",
        )
        observe_outputs(flat, children)

    class RuntimeObserver(EvaluationLifeCycleObserver):
        def on_after_start_graph(self, graph):
            if graph.parent_node is None and control.empty_runtime_rss_bytes is None:
                control.empty_runtime_rss_bytes = current_rss_bytes()

        def on_before_stop_graph(self, graph):
            if graph.parent_node is None and control.final_stop_start_ns is None:
                control.begin_final_stop()

        def on_after_stop_graph(self, graph):
            if graph.parent_node is None and control.after_stop_rss_bytes is None:
                stop_ns = time.perf_counter_ns()
                if control.final_stop_start_ns is not None:
                    control.final_stop_ns = stop_ns - control.final_stop_start_ns
                control.after_stop_rss_bytes = current_rss_bytes()

    observer = RuntimeObserver()
    logger = logging.getLogger("hgraph.high_count_map")
    logger.setLevel(logging.WARNING)
    config = GraphConfiguration(
        run_mode=EvaluationMode.SIMULATION,
        life_cycle_observers=(observer,),
        graph_logger=logger,
        default_log_level=logging.WARNING,
    )

    wiring_start_ns = time.perf_counter_ns()
    with WiringNodeInstanceContext(error_capture_options=config.error_capture_options):
        graph_builder = wire_graph(benchmark_graph, cycles)
    wiring_ns = time.perf_counter_ns() - wiring_start_ns
    control.after_wiring_rss_bytes = current_rss_bytes()

    run_start_ns = time.perf_counter_ns()
    evaluate_graph(graph_builder, config)
    run_ns = time.perf_counter_ns() - run_start_ns

    actual = control.actual
    expected = workload.expected
    checksum_valid = actual == expected
    profile = workload.selected_profile
    active_inner_graphs = profile.active_inner_graphs
    populated_rss_delta_bytes = (
        control.populated_rss_bytes - control.empty_runtime_rss_bytes
        if control.populated_rss_bytes is not None and control.empty_runtime_rss_bytes is not None
        else None
    )
    measured_cycles_ns = (
        control.measured_end_ns - control.measured_start_ns
        if control.measured_start_ns is not None and control.measured_end_ns is not None
        else None
    )
    current_rss_samples = [
        control.baseline_rss_bytes,
        control.after_wiring_rss_bytes,
        control.empty_runtime_rss_bytes,
        control.populated_rss_bytes,
        control.after_stop_rss_bytes,
    ]
    reported_peak_rss_bytes = max([peak_rss_bytes(), *(sample for sample in current_rss_samples if sample is not None)])
    latency = {
        name: statistics_for(control.cycle_latencies_ns.get(name, []))
        for name in (
            "ordinary",
            "flat_parent_removal",
            "flat_parent_restoration_cleanup",
            "child_removal",
            "child_restoration_cleanup",
            "broadcast",
        )
    }
    result: dict[str, Any] = {
        "schema_version": 1,
        "benchmark": "portable_high_count_map",
        "engine": f"hgraph-{HGRAPH_05_VERSION}",
        "runtime": "python",
        "source_commit": source_commit,
        "benchmark_adapter_commit": git_commit(),
        "profile": profile_name,
        "value_type": "int64_nonnegative",
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpu_count": os.cpu_count(),
            "cpu_affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
            "python": sys.version.split()[0],
        },
        "notes": [
            "hgraph 0.5 combines runtime construction and start inside evaluate_graph.",
            "Population timing covers source population and output observation; runtime start has no separate public hook.",
        ],
        "topology": {
            "flat_items": profile.flat_count,
            "parents": profile.parent_count,
            "children_per_parent": CHILD_COUNT,
            "active_inner_graphs": active_inner_graphs,
        },
        "workload": {
            "cycles": cycles,
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
            "wiring_ns": wiring_ns,
            "runtime_factory_ns": None,
            "runtime_start_ns": None,
            "population_ns": control.population_ns,
            "measured_cycles_ns": measured_cycles_ns,
            "wall_run_ns": run_ns,
            "final_stop_ns": control.final_stop_ns,
        },
        "throughput": {
            "cycles_per_second": cycles * 1e9 / measured_cycles_ns if measured_cycles_ns else None,
            "output_events_per_second": actual.count * 1e9 / measured_cycles_ns if measured_cycles_ns else None,
        },
        "latency": latency,
        "output": {
            "actual": {"count": actual.count, "sum": actual.sum, "xor": actual.xor_value},
            "expected": {"count": expected.count, "sum": expected.sum, "xor": expected.xor_value},
            "valid": checksum_valid,
        },
        "memory": {
            "baseline_rss_bytes": control.baseline_rss_bytes,
            "after_wiring_rss_bytes": control.after_wiring_rss_bytes,
            "empty_runtime_rss_bytes": control.empty_runtime_rss_bytes,
            "populated_rss_bytes": control.populated_rss_bytes,
            "peak_rss_bytes": reported_peak_rss_bytes,
            "after_stop_rss_bytes": control.after_stop_rss_bytes,
            "populated_rss_delta_bytes": populated_rss_delta_bytes,
            "bytes_per_active_inner_graph": (
                populated_rss_delta_bytes / active_inner_graphs if populated_rss_delta_bytes is not None else None
            ),
        },
    }

    print(json.dumps(result, sort_keys=True))
    return 0 if checksum_valid else 2


if __name__ == "__main__":
    raise SystemExit(main())

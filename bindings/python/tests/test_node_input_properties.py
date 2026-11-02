import pytest

import hgraph as hg


def test_compute_node_activity_selects_scheduling_inputs_and_samples_passive_values():
    calls = []
    starts = []

    @hg.compute_node(active=("trigger",))
    def combine(trigger: hg.TS[int], sample: hg.TS[int]) -> hg.TS[int]:
        calls.append((trigger.value, sample.value))
        return trigger.value + sample.value

    @combine.start
    def start():
        starts.append(True)

    @hg.graph
    def graph(trigger: hg.TS[int], sample: hg.TS[int]) -> hg.TS[int]:
        return combine(trigger, sample)

    assert hg.run_graph(graph, [1, None, 3], [10, 20, None]) == [11, None, 23]
    assert calls == [(1, 10), (3, 20)]
    assert starts == [True]


def test_outputless_compute_node_uses_configured_activity_and_validity():
    calls = []

    @hg.compute_node(active=("trigger",), valid=("trigger",))
    def observe(trigger: hg.TS[int], sample: hg.TS[int]) -> None:
        calls.append((trigger.value, sample.value if sample.valid else None))

    @hg.graph
    def graph(trigger: hg.TS[int], sample: hg.TS[int]) -> None:
        observe(trigger, hg.opt(sample))

    assert hg.run_graph(graph, [1, None, 2], [None, 10, None]) is None
    assert calls == [(1, None), (2, 10)]


def test_empty_activity_tuple_selects_no_active_inputs():
    calls = []

    @hg.compute_node(active=(), valid=())
    def observe(value: hg.TS[int]) -> hg.TS[int]:
        calls.append(value)
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return observe(value)

    assert hg.run_graph(graph, [1, 2]) == [None, None]
    assert calls == []


def test_empty_validity_tuple_makes_every_input_optional():
    calls = []

    @hg.compute_node(active=("trigger",), valid=())
    def observe(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        calls.append((trigger.value, value.value if value.valid else None))
        return trigger.value

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return observe(trigger, hg.opt(value))

    assert hg.run_graph(graph, [1, 2], [None, 5]) == [1, 2]
    assert calls == [(1, None), (2, 5)]


def test_omitted_activity_and_validity_preserve_required_active_defaults():
    calls = []

    @hg.compute_node
    def combine(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        calls.append((trigger.value, value.value))
        return trigger.value + value.value

    @hg.graph
    def graph(trigger: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return combine(trigger, value)

    assert hg.run_graph(graph, [1, None], [None, 2]) == [None, 3]
    assert calls == [(1, 2)]


def test_compute_node_validity_selects_required_inputs_and_accepts_opt_for_others():
    calls = []

    @hg.compute_node(active=("trigger",), valid=("trigger",))
    def combine(trigger: hg.TS[int], sample: hg.TS[int]) -> hg.TS[int]:
        calls.append((trigger.value, sample.value if sample.valid else None))
        return trigger.value + (sample.value if sample.valid else 0)

    @hg.graph
    def graph(trigger: hg.TS[int], sample: hg.TS[int]) -> hg.TS[int]:
        return combine(trigger, hg.opt(sample))

    assert hg.run_graph(graph, [1, None, 2], [None, 10, None]) == [1, None, 12]
    assert calls == [(1, None), (2, 10)]


def test_compute_node_rejects_opt_for_a_selected_required_input():
    @hg.compute_node(valid=("sample",))
    def identity(sample: hg.TS[int]) -> hg.TS[int]:
        return sample.value

    @hg.graph
    def graph(sample: hg.TS[int]) -> hg.TS[int]:
        return identity(hg.opt(sample))

    with pytest.raises(ValueError, match="an opt-qualified argument requires an optional input parameter"):
        hg.run_graph(graph, [1])


def test_compute_node_all_valid_gates_on_every_fixed_list_item():
    calls = []

    @hg.compute_node(active=("trigger",), valid=("trigger",), all_valid=("values",))
    def total(trigger: hg.TS[int], values: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        calls.append(trigger.value)
        return sum(values.value)

    @hg.graph
    def graph(trigger: hg.TS[int], values: hg.TSL[hg.TS[int], 2]) -> hg.TS[int]:
        return total(trigger, values)

    assert hg.run_graph(graph, [1, 2], [{0: 10}, {1: 20}]) == [None, 30]
    assert calls == [2]


def test_node_properties_apply_to_time_series_resolutions_and_preserve_optional_override():
    calls = []

    @hg.node(active=("trigger",), valid=("trigger", "sample"))
    def combine(trigger: int, sample: int | None) -> int:
        calls.append((trigger, sample))
        return trigger + (sample or 0)

    @hg.graph
    def graph(trigger: hg.TS[int], sample: hg.TS[int]) -> hg.TS[int]:
        return combine(trigger, sample)

    assert hg.run_graph(graph, [1, 2], [None, 5]) == [None, 7]
    assert calls == [(2, 5)]

    @hg.graph
    def scalar_graph() -> hg.TS[int]:
        return combine(1, 5)

    assert hg.run_graph(scalar_graph) == [6]


@pytest.mark.parametrize(
    ("decorator", "options", "message"),
    [
        (hg.compute_node, {"active": ("missing",)}, "unknown input"),
        (hg.compute_node, {"active": (1,)}, "entries must be input names"),
        (hg.compute_node, {"valid": ["value"]}, "must be a tuple"),
        (hg.compute_node, {"active": ("value",)}, "time-series-capable input"),
        (hg.compute_node, {"active": ("scheduler",)}, "injectable parameter"),
        (hg.node, {"active": ("scheduler",)}, "injectable parameter"),
    ],
)
def test_node_input_properties_reject_invalid_declarations(decorator, options, message):
    with pytest.raises(TypeError, match=message):
        @decorator(**options)
        def invalid(value: int, scheduler: hg.Scheduler = None) -> int:
            return value


def test_node_input_properties_reject_valid_and_all_valid_overlap():
    with pytest.raises(TypeError, match="both valid and all_valid"):
        @hg.compute_node(valid=("value",), all_valid=("value",))
        def invalid(value: hg.TS[int]) -> hg.TS[int]:
            return value.value


def test_node_input_properties_reject_scalar_only_parameters():
    with pytest.raises(TypeError, match="time-series-capable input"):
        @hg.node(active=("value",))
        def invalid(value: hg.Scalar[int]) -> int:
            return value

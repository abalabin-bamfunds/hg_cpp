import pytest

import hgraph as hg


@hg.node
def add(left: int, right: int) -> int:
    return left + right


@hg.node
def add_amount(value: int, amount: int) -> int:
    return value + amount


@hg.compute_node
def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
    return {1: trigger.value, 2: trigger.value * 10}


def test_map_wires_python_graphs_and_passes_the_key():
    @hg.graph
    def branch(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return add(key, value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(branch, make_values(trigger), __key_arg__=0)
        return mapped[2]

    assert hg.run_graph(graph, [1, 3]) == [12, 32]


def test_map_maps_scalar_dict_snapshots_with_named_arguments():
    @hg.compute_node
    def make_dictionary(trigger: hg.TS[int]) -> hg.TS[dict[int, int]]:
        return {1: trigger.value, 2: trigger.value * 10}

    @hg.graph
    def add_named_amount(value: hg.TS[int], amount: int) -> hg.TS[int]:
        return add_amount(value, amount)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(add_named_amount, make_dictionary(trigger), amount=5)[2]

    assert hg.run_graph(graph, [1, 2]) == [15, 25]


def test_map_uses_the_union_of_mapped_dictionaries():
    @hg.compute_node
    def make_left(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value}

    @hg.compute_node
    def make_right(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {2: trigger.value * 10}

    @hg.graph
    def available(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return right

    @hg.compute_node
    def count_branches(values: hg.TSD[int, hg.REF[hg.TS[int]]]) -> hg.TS[int]:
        return len(values)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(available, make_left(trigger), make_right(trigger))
        return mapped[2]

    @hg.graph
    def branch_count(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(available, make_left(trigger), make_right(trigger))
        return count_branches(mapped)

    assert hg.run_graph(graph, [1, 3]) == [10, 30]
    assert hg.run_graph(branch_count, [1]) == [2]


def test_map_selects_a_named_key_set_argument():
    @hg.compute_node
    def make_left(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value}

    @hg.compute_node
    def make_right(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {2: trigger.value}

    @hg.graph
    def branch(key: hg.TS[int], left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return key

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(
            branch,
            left=make_left(trigger),
            right=make_right(trigger),
            __key_arg__="key",
            __key_set_arg__="left",
        )
        return mapped[2]

    @hg.graph
    def selected_left_value(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(
            branch,
            left=make_left(trigger),
            right=make_right(trigger),
            __key_arg__="key",
            __key_set_arg__="left",
        )
        return mapped[1]

    assert hg.run_graph(graph, [1, 2]) == [None, None]
    assert hg.run_graph(selected_left_value, [1]) == [1]


def test_map_pass_through_preserves_the_complete_time_series():
    @hg.graph
    def branch(value: hg.TS[int], factor: hg.TS[int]) -> hg.TS[int]:
        return add(value, factor)

    @hg.graph
    def graph(trigger: hg.TS[int], factor: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(
            branch,
            factor=hg.pass_through(factor),
            value=make_values(trigger),
        )
        return mapped[1]

    assert hg.run_graph(graph, [2, 3], [10, 20]) == [12, 23]


def test_map_preserves_named_arguments_and_selects_a_named_key_parameter():
    @hg.graph
    def branch(value: hg.TS[int], key: hg.TS[int]) -> hg.TS[int]:
        return add(value, key)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(branch, value=make_values(trigger), __key_arg__="key")
        return mapped[2]

    assert hg.run_graph(graph, [1, 3]) == [12, 32]


def test_nested_map_uses_normal_graph_wiring():
    @hg.compute_node
    def make_inner(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {5: value.value}

    @hg.graph
    def inner_branch(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 1)

    @hg.graph
    def outer_branch(value: hg.TS[int]) -> hg.TS[int]:
        return hg.map(inner_branch, make_inner(value))[5]

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(outer_branch, make_values(trigger))[2]

    assert hg.run_graph(graph, [1, 3]) == [11, 31]


def test_map_removes_and_recreates_a_branch_on_later_cycles():
    @hg.compute_node
    def changing_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 2:
            return {1: hg.REMOVE}
        return {1: trigger.value}

    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 10)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(branch, changing_values(trigger))[1]

    assert hg.run_graph(graph, [1, 2, 3]) == [11, None, 13]


def test_map_supports_source_and_sink_graphs():
    events = []

    @hg.compute_node
    def keys(trigger: hg.TS[int]) -> hg.TSS[int]:
        if trigger.value == 1:
            return hg.SetDelta(added={7})
        return hg.SetDelta(removed={7})

    @hg.compute_node
    def copy(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.compute_node
    def record(value: hg.TS[int]) -> None:
        events.append(value.value)

    @hg.graph
    def source_branch(key: hg.TS[int]) -> hg.TS[int]:
        return copy(key)

    @hg.graph
    def sink_branch(value: hg.TS[int]) -> None:
        record(value)

    @hg.graph
    def source_graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(source_branch, keys(trigger), __key_arg__=0)[7]

    @hg.graph
    def sink_graph(trigger: hg.TS[int]) -> hg.TS[int]:
        assert hg.map(sink_branch, make_values(trigger)) is None
        return trigger

    assert hg.run_graph(source_graph, [1, 2, 1]) == [7, None, 7]
    assert hg.run_graph(sink_graph, [2, 3]) == [2, 3]
    assert events == [2, 20, 3, 30]


def test_switch_selects_cases_tuple_keys_and_default():
    @hg.graph
    def first(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 10)

    @hg.graph
    def second(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 20)

    @hg.graph
    def fallback(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 100)

    @hg.graph
    def graph(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(
            key,
            {(1, 2): first, 3: second, hg.DEFAULT: fallback},
            value,
        )

    assert hg.run_graph(graph, [1, 2, 3, 9], [5, 6, 7, 8]) == [15, 16, 27, 108]


def test_switch_accepts_an_optional_key_wiring_point():
    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 10)

    @hg.graph
    def graph(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(hg.opt(key), {1: branch}, value)

    assert hg.run_graph(graph, [1, 1], [2, 5]) == [12, 15]


def test_switch_reports_an_unmatched_key():
    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def graph(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: branch}, value)

    with pytest.raises(IndexError, match="does not match a case"):
        hg.run_graph(graph, [2], [5])


def test_switch_matches_named_arguments_for_each_case():
    @hg.graph
    def use_value(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def use_other(other: hg.TS[int]) -> hg.TS[int]:
        return other

    @hg.graph
    def graph(key: hg.TS[int], value: hg.TS[int], other: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: use_value, 2: use_other}, value=value, other=other)

    assert hg.run_graph(graph, [1, 2], [10, 20], [100, 200]) == [10, 200]


def test_switch_supports_source_and_sink_case_graphs():
    events = []

    @hg.graph
    def source() -> hg.TS[int]:
        return hg.operators.const_(42)

    @hg.compute_node
    def record(value: hg.TS[int]) -> None:
        events.append(value.value)

    @hg.graph
    def sink(value: hg.TS[int]) -> None:
        record(value)

    @hg.graph
    def source_graph(key: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: source})

    @hg.graph
    def sink_graph(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        assert hg.switch_(key, {1: sink}, value) is None
        return value

    assert hg.run_graph(source_graph, [1]) == [42]
    assert hg.run_graph(sink_graph, [1, 1], [7, 9]) == [7, 9]
    assert events == [7, 9]


def test_switch_inside_map_recreates_the_selected_inner_graph():
    @hg.graph
    def first(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 10)

    @hg.graph
    def second(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 20)

    @hg.graph
    def mapped_branch(value: hg.TS[int], selected: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(selected, {1: first, 2: second}, value)

    @hg.graph
    def graph(trigger: hg.TS[int], selected: hg.TS[int]) -> hg.TS[int]:
        mapped = hg.map(mapped_branch, make_values(trigger), hg.pass_through(selected))
        return mapped[1]

    assert hg.run_graph(graph, [1, 2, 3], [1, 2, 1]) == [11, 22, 13]


def test_mesh_evaluates_a_requested_branch_first():
    @hg.graph
    def leaf(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def dependent(value: hg.TS[int]) -> hg.TS[int]:
        return add(value, hg.mesh()[1])

    @hg.graph
    def branch(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: leaf, 2: dependent}, value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        result = hg.mesh(branch, make_values(trigger), __key_arg__=0)
        return result[2]

    assert hg.run_graph(graph, [1, 3]) == [11, 33]


def test_mesh_creates_a_branch_for_a_requested_missing_key():
    @hg.compute_node
    def dependencies(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {2: 1}

    @hg.compute_node
    def observe(value: hg.TS[int]) -> None:
        pass

    @hg.graph
    def branch(key: hg.TS[int], dependency_key: hg.TS[int]) -> hg.TS[int]:
        observe(hg.mesh()[dependency_key])
        return key

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        result = hg.mesh(branch, dependencies(trigger), __key_arg__=0)
        return result[1]

    assert hg.run_graph(graph, [1]) == [1]


def test_named_outer_mesh_can_be_selected_from_an_inner_mesh():
    @hg.compute_node
    def make_single(value: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {5: value.value}

    @hg.graph
    def inner_branch(value: hg.TS[int]) -> hg.TS[int]:
        return add(value, hg.mesh("outer")[1])

    @hg.graph
    def leaf(value: hg.TS[int]) -> hg.TS[int]:
        return value

    @hg.graph
    def nested(value: hg.TS[int]) -> hg.TS[int]:
        return hg.mesh(inner_branch, make_single(value), name="inner")[5]

    @hg.graph
    def outer_branch(key: hg.TS[int], value: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: leaf, 2: nested}, value)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        result = hg.mesh(outer_branch, make_values(trigger), name="outer", __key_arg__=0)
        return result[2]

    assert hg.run_graph(graph, [1, 3]) == [11, 33]


def test_mesh_removes_and_recreates_a_branch_on_later_cycles():
    @hg.compute_node
    def changing_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        if trigger.value == 2:
            return {1: hg.REMOVE}
        return {1: trigger.value}

    @hg.graph
    def branch(value: hg.TS[int]) -> hg.TS[int]:
        return add_amount(value, 10)

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.mesh(branch, changing_values(trigger))[1]

    assert hg.run_graph(graph, [1, 2, 3]) == [11, None, 13]

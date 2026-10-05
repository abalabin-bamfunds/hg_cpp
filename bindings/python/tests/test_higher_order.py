import pytest

import bg


@bg.compute_node
def add(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
    return left.value + right.value


@bg.compute_node
def add_amount(value: bg.TS[int], amount: int) -> bg.TS[int]:
    return value.value + amount


@bg.compute_node
def make_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
    return {1: trigger.value, 2: trigger.value * 10}


def test_map_wires_python_graphs_and_passes_the_key():
    @bg.graph
    def branch(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return add(key, value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(branch, make_values(trigger), __key_arg__=0)
        return mapped[2]

    assert bg.test_graph(graph, [1, 3]) == [12, 32]


def test_map_uses_the_union_of_mapped_dictionaries():
    @bg.compute_node
    def make_left(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: trigger.value}

    @bg.compute_node
    def make_right(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {2: trigger.value * 10}

    @bg.graph
    def available(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return right

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(available, make_left(trigger), make_right(trigger))
        return mapped[2]

    assert bg.test_graph(graph, [1, 3]) == [10, 30]


def test_map_selects_a_named_key_set_argument():
    @bg.compute_node
    def make_left(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: trigger.value}

    @bg.compute_node
    def make_right(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {2: trigger.value}

    @bg.graph
    def branch(key: bg.TS[int], left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return key

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(
            branch,
            left=make_left(trigger),
            right=make_right(trigger),
            __key_arg__="key",
            __key_set_arg__="left",
        )
        return mapped[2]

    assert bg.test_graph(graph, [1, 2]) == [None, None]


def test_map_pass_through_preserves_the_complete_time_series():
    @bg.graph
    def branch(value: bg.TS[int], factor: bg.TS[int]) -> bg.TS[int]:
        return add(value, factor)

    @bg.graph
    def graph(trigger: bg.TS[int], factor: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(
            branch,
            factor=bg.pass_through(factor),
            value=make_values(trigger),
        )
        return mapped[1]

    assert bg.test_graph(graph, [2, 3], [10, 20]) == [12, 23]


def test_map_preserves_named_arguments_and_selects_a_named_key_parameter():
    @bg.graph
    def branch(value: bg.TS[int], key: bg.TS[int]) -> bg.TS[int]:
        return add(value, key)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(branch, value=make_values(trigger), __key_arg__="key")
        return mapped[2]

    assert bg.test_graph(graph, [1, 3]) == [12, 32]


def test_nested_map_uses_normal_graph_wiring():
    @bg.compute_node
    def make_inner(value: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {5: value.value}

    @bg.graph
    def inner_branch(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 1)

    @bg.graph
    def outer_branch(value: bg.TS[int]) -> bg.TS[int]:
        return bg.map(inner_branch, make_inner(value))[5]

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.map(outer_branch, make_values(trigger))[2]

    assert bg.test_graph(graph, [1, 3]) == [11, 31]


def test_map_removes_and_recreates_a_branch_on_later_cycles():
    @bg.compute_node
    def changing_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 2:
            return {1: bg.REMOVE}
        return {1: trigger.value}

    @bg.graph
    def branch(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 10)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.map(branch, changing_values(trigger))[1]

    assert bg.test_graph(graph, [1, 2, 3]) == [11, None, 13]


def test_map_supports_source_and_sink_graphs():
    events = []

    @bg.compute_node
    def keys(trigger: bg.TS[int]) -> bg.TSS[int]:
        if trigger.value == 1:
            return bg.SetDelta(added={7})
        return bg.SetDelta(removed={7})

    @bg.compute_node
    def copy(value: bg.TS[int]) -> bg.TS[int]:
        return value.value

    @bg.compute_node
    def record(value: bg.TS[int]) -> None:
        events.append(value.value)

    @bg.graph
    def source_branch(key: bg.TS[int]) -> bg.TS[int]:
        return copy(key)

    @bg.graph
    def sink_branch(value: bg.TS[int]) -> None:
        record(value)

    @bg.graph
    def source_graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.map(source_branch, keys(trigger), __key_arg__=0)[7]

    @bg.graph
    def sink_graph(trigger: bg.TS[int]) -> bg.TS[int]:
        assert bg.map(sink_branch, make_values(trigger)) is None
        return trigger

    assert bg.test_graph(source_graph, [1, 2, 1]) == [7, None, 7]
    assert bg.test_graph(sink_graph, [2, 3]) == [2, 3]
    assert events == [2, 20, 3, 30]


def test_switch_selects_cases_tuple_keys_and_default():
    @bg.graph
    def first(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 10)

    @bg.graph
    def second(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 20)

    @bg.graph
    def fallback(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 100)

    @bg.graph
    def graph(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(
            key,
            {(1, 2): first, 3: second, bg.DEFAULT: fallback},
            value,
        )

    assert bg.test_graph(graph, [1, 2, 3, 9], [5, 6, 7, 8]) == [15, 16, 27, 108]


def test_switch_accepts_an_optional_key_wiring_point():
    @bg.graph
    def branch(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 10)

    @bg.graph
    def graph(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(bg.opt(key), {1: branch}, value)

    assert bg.test_graph(graph, [1, 1], [2, 5]) == [12, 15]


def test_switch_reports_an_unmatched_key():
    @bg.graph
    def branch(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def graph(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(key, {1: branch}, value)

    with pytest.raises(IndexError, match="does not match a case"):
        bg.test_graph(graph, [2], [5])


def test_switch_matches_named_arguments_for_each_case():
    @bg.graph
    def use_value(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def use_other(other: bg.TS[int]) -> bg.TS[int]:
        return other

    @bg.graph
    def graph(key: bg.TS[int], value: bg.TS[int], other: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(key, {1: use_value, 2: use_other}, value=value, other=other)

    assert bg.test_graph(graph, [1, 2], [10, 20], [100, 200]) == [10, 200]


def test_switch_supports_source_and_sink_case_graphs():
    events = []

    @bg.graph
    def source() -> bg.TS[int]:
        return bg.operators.const_(42)

    @bg.compute_node
    def record(value: bg.TS[int]) -> None:
        events.append(value.value)

    @bg.graph
    def sink(value: bg.TS[int]) -> None:
        record(value)

    @bg.graph
    def source_graph(key: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(key, {1: source})

    @bg.graph
    def sink_graph(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        assert bg.switch_(key, {1: sink}, value) is None
        return value

    assert bg.test_graph(source_graph, [1]) == [42]
    assert bg.test_graph(sink_graph, [1, 1], [7, 9]) == [7, 9]
    assert events == [7, 9]


def test_switch_inside_map_recreates_the_selected_inner_graph():
    @bg.graph
    def first(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 10)

    @bg.graph
    def second(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 20)

    @bg.graph
    def mapped_branch(value: bg.TS[int], selected: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(selected, {1: first, 2: second}, value)

    @bg.graph
    def graph(trigger: bg.TS[int], selected: bg.TS[int]) -> bg.TS[int]:
        mapped = bg.map(mapped_branch, make_values(trigger), bg.pass_through(selected))
        return mapped[1]

    assert bg.test_graph(graph, [1, 2, 3], [1, 2, 1]) == [11, 22, 13]


def test_mesh_evaluates_a_requested_branch_first():
    @bg.graph
    def leaf(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def dependent(value: bg.TS[int]) -> bg.TS[int]:
        return add(value, bg.mesh()[1])

    @bg.graph
    def branch(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(key, {1: leaf, 2: dependent}, value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        result = bg.mesh(branch, make_values(trigger), __key_arg__=0)
        return result[2]

    assert bg.test_graph(graph, [1, 3]) == [11, 33]


def test_mesh_creates_a_branch_for_a_requested_missing_key():
    @bg.compute_node
    def dependencies(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {2: 1}

    @bg.compute_node
    def observe(value: bg.TS[int]) -> None:
        pass

    @bg.graph
    def branch(key: bg.TS[int], dependency_key: bg.TS[int]) -> bg.TS[int]:
        observe(bg.mesh()[dependency_key])
        return key

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        result = bg.mesh(branch, dependencies(trigger), __key_arg__=0)
        return result[1]

    assert bg.test_graph(graph, [1]) == [1]


def test_named_outer_mesh_can_be_selected_from_an_inner_mesh():
    @bg.compute_node
    def make_single(value: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {5: value.value}

    @bg.graph
    def inner_branch(value: bg.TS[int]) -> bg.TS[int]:
        return add(value, bg.mesh("outer")[1])

    @bg.graph
    def leaf(value: bg.TS[int]) -> bg.TS[int]:
        return value

    @bg.graph
    def nested(value: bg.TS[int]) -> bg.TS[int]:
        return bg.mesh(inner_branch, make_single(value), name="inner")[5]

    @bg.graph
    def outer_branch(key: bg.TS[int], value: bg.TS[int]) -> bg.TS[int]:
        return bg.switch_(key, {1: leaf, 2: nested}, value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        result = bg.mesh(outer_branch, make_values(trigger), name="outer", __key_arg__=0)
        return result[2]

    assert bg.test_graph(graph, [1, 3]) == [11, 33]


def test_mesh_removes_and_recreates_a_branch_on_later_cycles():
    @bg.compute_node
    def changing_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        if trigger.value == 2:
            return {1: bg.REMOVE}
        return {1: trigger.value}

    @bg.graph
    def branch(value: bg.TS[int]) -> bg.TS[int]:
        return add_amount(value, 10)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.mesh(branch, changing_values(trigger))[1]

    assert bg.test_graph(graph, [1, 2, 3]) == [11, None, 13]

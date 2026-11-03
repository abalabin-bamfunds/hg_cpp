import pytest

import hgraph as hg


@hg.compute_node
def add_context(value: hg.TS[int], offset: hg.CONTEXT[hg.TS[int]] = None, amount: int = 0) -> hg.TS[int]:
    return value.value + offset.value + amount


@hg.compute_node
def add_context_only(value: hg.TS[int], offset: hg.CONTEXT[hg.TS[int]] = None) -> hg.TS[int]:
    return value.value + offset.value


@hg.compute_node
def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
    return {1: trigger.value}


def test_context_node_input_uses_the_name_assigned_by_with_as():
    @hg.graph
    def graph(value: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            return add_context(value)

    assert hg.run_graph(graph, [1, 2], [10, 20]) == [11, 22]


def test_ordinary_positional_argument_after_context_input_keeps_its_binding():
    @hg.graph
    def graph(value: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            return add_context(value, 5)

    assert hg.run_graph(graph, [1], [10]) == [16]


def test_context_graph_input_is_available_while_wiring_its_body():
    @hg.graph
    def inner(value: hg.TS[int], offset: hg.CONTEXT[hg.TS[int]] = None) -> hg.TS[int]:
        return add_context(value)

    @hg.graph
    def outer(value: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            return inner(value)

    assert hg.run_graph(outer, [1, 2], [10, 20]) == [11, 22]


def test_context_input_can_be_overridden_by_name_and_cannot_bind_positionally():
    @hg.graph
    def explicit(value: hg.TS[int], source: hg.TS[int], override: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            return add_context(value, offset=override)

    @hg.graph
    def positional(value: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        return add_context_only(value, source)

    assert hg.run_graph(explicit, [1, 2], [10, 20], [100, 200]) == [101, 202]
    with pytest.raises(TypeError, match="positional"):
        hg.run_graph(positional, [1], [10])


def test_missing_context_input_fails_during_wiring():
    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return add_context(value)

    with pytest.raises(ValueError, match="required context 'offset' is not available"):
        hg.run_graph(graph, [1])


def test_context_is_removed_after_the_context_body_raises_during_wiring():
    @hg.graph
    def failing(value: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            add_context(value)
            raise RuntimeError("wiring failed")

    @hg.graph
    def missing(value: hg.TS[int]) -> hg.TS[int]:
        return add_context(value)

    with pytest.raises(RuntimeError, match="wiring failed"):
        hg.run_graph(failing, [1], [10])

    with pytest.raises(ValueError, match="required context 'offset' is not available"):
        hg.run_graph(missing, [1])


def test_nested_context_scopes_restore_the_outer_binding():
    @hg.graph
    def graph(value: hg.TS[int], outer: hg.TS[int], inner: hg.TS[int]) -> hg.TS[int]:
        with outer as offset:
            with inner as offset:
                inner_value = add_context(value)
            outer_value = add_context(value)
            return inner_value + outer_value

    assert hg.run_graph(graph, [1], [10], [100]) == [112]


def test_context_input_crosses_a_map_graph_boundary():
    @hg.graph
    def branch(key: hg.TS[int], value: hg.TS[int], offset: hg.CONTEXT[hg.TS[int]] = None) -> hg.TS[int]:
        return add_context(value)

    @hg.graph
    def graph(trigger: hg.TS[int], source: hg.TS[int]) -> hg.TS[int]:
        with source as offset:
            mapped = hg.map(branch, make_values(trigger), __key_arg__=0)
            return mapped[1]

    assert hg.run_graph(graph, [1, 2], [10, 20]) == [11, 22]

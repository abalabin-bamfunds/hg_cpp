import inspect

import bg
import pytest


@bg.compute_node
def add(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
    return left.value + right.value


def test_graph_var_kwargs_are_packed_as_a_generic_tsb():
    @bg.graph
    def select_right(**bundle: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return bundle["right"]

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return select_right(left=left, right=right)

    assert bg.test_graph(graph, [1, 2], [3, 4]) == [3, 4]


def test_graph_var_kwargs_are_packed_as_a_declared_tsb():
    class Pair(bg.TimeSeriesSchema):
        left: bg.TS[int]
        right: bg.TS[int]

    @bg.graph
    def select_right(**bundle: bg.TSB[Pair]) -> bg.TS[int]:
        return bundle["right"]

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return select_right(left=left, right=right)

    assert bg.test_graph(graph, [1], [2]) == [2]


def test_graph_var_args_are_packed_as_a_tsl():
    @bg.graph
    def select_second(*values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
        return values[1]

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return select_second(left, right)

    assert bg.test_graph(graph, [1, 2], [3, 4]) == [3, 4]


def test_variadic_tsl_retains_a_generic_bundle_item_annotation():
    class Pair(bg.TimeSeriesSchema):
        left: bg.TS[int]
        right: bg.TS[int]

    @bg.compute_node
    def make_pair(value: bg.TS[int]) -> bg.TSB[Pair]:
        return {"left": value.value, "right": value.value * 10}

    @bg.graph
    def select_second(*values: bg.TSL[bg.TSB[bg.TS_SCHEMA], bg.SIZE]) -> bg.TS[int]:
        return bg.operators.getattr(values[1], bg.symbol("right"))

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        pair = make_pair(value)
        return select_second(pair, pair)

    assert bg.test_graph(graph, [1, 3]) == [10, 30]


def test_graph_var_args_can_capture_a_heterogeneous_tsb():
    @bg.compute_node
    def use_options(value: bg.TS[int], amount: int, factor: float) -> bg.TS[int]:
        return int((value.value + amount) * factor)

    @bg.graph
    def apply(*arguments: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return use_options(arguments[0], arguments[1], arguments[2])

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return apply(value, 2, 3.0)

    assert bg.test_graph(graph, [1, 2]) == [9, 12]


def test_node_receives_materialized_variadic_collections():
    @bg.compute_node
    def total(
        first: bg.TS[int],
        *values: bg.TSL[bg.TS[int], bg.SIZE],
        offset: bg.TS[int],
        **more: bg.TSB[bg.TS_SCHEMA],
    ) -> bg.TS[int]:
        return first.value + sum(values.value) + offset.value + sum(more.value.values())

    @bg.graph
    def graph(first: bg.TS[int], second: bg.TS[int], offset: bg.TS[int]) -> bg.TS[int]:
        return total(first, second, offset=offset, bonus=5)

    assert bg.test_graph(graph, [1, 2], [10, 20], [100, 200]) == [116, 227]


def test_positional_and_named_tsb_packs_resolve_independently():
    @bg.compute_node
    def total(
        *positional: bg.TSB[bg.TS_SCHEMA],
        **named: bg.TSB[bg.TS_SCHEMA],
    ) -> bg.TS[int]:
        return sum(positional.value.values()) + sum(named.value.values())

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return total(value, 2, amount=3)

    assert bg.test_graph(graph, [1, 4]) == [6, 9]


def test_empty_variadic_collections_tick_at_graph_start():
    @bg.compute_node
    def count_list(*values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
        return len(values.value)

    @bg.compute_node
    def count_bundle(**values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return len(values.value)

    @bg.graph
    def graph() -> bg.TS[int]:
        return add(count_list(), count_bundle())

    assert bg.test_graph(graph) == [0]


def test_variadic_tsb_can_be_forwarded_through_a_nested_graph():
    @bg.compute_node
    def total(**values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return sum(values.value.values())

    @bg.graph
    def forward(**values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return total(values=values)

    @bg.graph
    def graph(left: bg.TS[int], right: bg.TS[int]) -> bg.TS[int]:
        return forward(left=left, right=right)

    assert bg.test_graph(graph, [1, 2], [10, 20]) == [11, 22]


def test_exact_pack_name_accepts_a_prebuilt_tsb():
    class Pair(bg.TimeSeriesSchema):
        left: bg.TS[int]
        right: bg.TS[int]

    @bg.compute_node
    def make_pair(value: bg.TS[int]) -> bg.TSB[Pair]:
        return {"left": value.value, "right": value.value * 10}

    @bg.graph
    def select_right(**bundle: bg.TSB[Pair]) -> bg.TS[int]:
        return bundle["right"]

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return select_right(bundle=make_pair(value))

    assert bg.test_graph(graph, [1, 3]) == [10, 30]


def test_exact_pack_name_accepts_a_prebuilt_tsl():
    @bg.compute_node
    def make_list(value: bg.TS[int]) -> bg.TSL[bg.TS[int], 2]:
        return {0: value.value, 1: value.value * 10}

    @bg.graph
    def select_second(*values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
        return values[1]

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return select_second(values=make_list(value))

    assert bg.test_graph(graph, [1, 3]) == [10, 30]


def test_map_can_invoke_a_variadic_python_graph():
    @bg.compute_node
    def make_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: trigger.value, 2: trigger.value * 10}

    @bg.graph
    def branch(**values: bg.TSB[bg.TS_SCHEMA]) -> bg.TS[int]:
        return values["value"]

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return bg.map(branch, value=make_values(trigger))[2]

    assert bg.test_graph(graph, [1, 3]) == [10, 30]


def test_variadic_sink_node_receives_updates():
    observed = []

    @bg.compute_node
    def record(**values: bg.TSB[bg.TS_SCHEMA]) -> None:
        observed.append(values.value)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        assert record(value=value, amount=2) is None
        return value

    assert bg.test_graph(graph, [1, 3]) == [1, 3]
    assert observed == [{"value": 1, "amount": 2}, {"value": 3, "amount": 2}]


def test_variadic_definition_preserves_its_python_signature():
    @bg.graph
    def definition(
        first: bg.TS[int],
        *values: bg.TSL[bg.TS[int], bg.SIZE],
        offset: int = 1,
        **options: bg.TSB[bg.TS_SCHEMA],
    ) -> bg.TS[int]:
        return first

    parameters = inspect.signature(definition).parameters

    assert parameters["values"].kind is inspect.Parameter.VAR_POSITIONAL
    assert parameters["offset"].kind is inspect.Parameter.KEYWORD_ONLY
    assert parameters["options"].kind is inspect.Parameter.VAR_KEYWORD


def test_fixed_keyword_only_parameters_are_passed_to_python_callbacks():
    @bg.compute_node
    def add_offset(value: bg.TS[int], *, offset: bg.TS[int]) -> bg.TS[int]:
        return value.value + offset.value

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return add_offset(value, offset=2)

    assert bg.test_graph(graph, [1, 3]) == [3, 5]


def test_variadic_tsl_rejects_scalars_and_mixed_time_series_types():
    @bg.compute_node
    def consume(*values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
        return sum(values.value)

    @bg.graph
    def scalar_graph(value: bg.TS[int]) -> bg.TS[int]:
        return consume(value, 2)

    @bg.graph
    def mixed_graph(integer: bg.TS[int], floating: bg.TS[float]) -> bg.TS[int]:
        return consume(integer, floating)

    with pytest.raises(ValueError, match="variadic TSL accepts only time series"):
        bg.test_graph(scalar_graph, [1])
    with pytest.raises(ValueError, match="compatible item types"):
        bg.test_graph(mixed_graph, [1], [2.0])


def test_variadic_parameters_require_collection_annotations():
    with pytest.raises(ValueError, match="requires a time-series collection"):
        @bg.graph
        def scalar_args(*values: int) -> bg.TS[int]:
            raise AssertionError

    with pytest.raises(ValueError, match="TSL parameter requires var_args"):
        @bg.graph
        def list_kwargs(**values: bg.TSL[bg.TS[int], bg.SIZE]) -> bg.TS[int]:
            raise AssertionError

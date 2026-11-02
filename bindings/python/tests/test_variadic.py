import inspect

import pytest

import hg


def test_run_graph_var_kwargs_are_packed_as_a_generic_tsb():
    @hg.graph
    def select_right(**bundle: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return bundle["right"]

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return select_right(left=left, right=right)

    assert hg.run_graph(graph, [1, 2], [3, 4]) == [3, 4]


def test_run_graph_var_kwargs_are_packed_as_a_declared_tsb():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.graph
    def select_right(**bundle: hg.TSB[Pair]) -> hg.TS[int]:
        return bundle["right"]

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return select_right(left=left, right=right)

    assert hg.run_graph(graph, [1], [2]) == [2]


def test_run_graph_var_args_are_packed_as_a_tsl():
    @hg.graph
    def select_second(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return values[1]

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return select_second(left, right)

    assert hg.run_graph(graph, [1, 2], [3, 4]) == [3, 4]


def test_run_graph_variadic_tsl_item_accepts_a_tick_sequence():
    @hg.graph
    def identity(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return values[0]

    assert hg.run_graph(identity, [1, 2]) == [1, 2]


def test_run_graph_variadic_tsb_field_accepts_a_tick_sequence():
    @hg.schema
    class Values:
        value: hg.TS[int]

    @hg.graph
    def identity(**values: hg.TSB[Values]) -> hg.TS[int]:
        return values["value"]

    assert hg.run_graph(identity, value=[1, 2]) == [1, 2]


def test_run_graph_generic_variadic_tsb_preserves_scalar_wiring():
    @hg.graph
    def identity(**values: hg.TSB[hg.TS_SCHEMA]) -> int:
        return values["value"]

    assert hg.run_graph(identity, value=3) == [3]


def test_run_graph_generic_variadic_tsb_rejects_tick_sequences_without_field_types():
    @hg.graph
    def identity(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return values["value"]

    with pytest.raises(ValueError, match="declared TSB field type"):
        hg.run_graph(identity, value=hg.ticks([1, 2]))


def test_variadic_collection_guarantee_requires_a_concrete_collection():
    with pytest.raises(ValueError, match="guarantee requires a concrete time-series type"):

        @hg.graph
        def positional(*values: hg.TSL[hg.TS[int], hg.SIZE, hg.final]) -> hg.TS[int]:
            return values[0]

    with pytest.raises(ValueError, match="guarantee requires a concrete time-series type"):

        @hg.graph
        def named(**values: hg.TSB[hg.TS_SCHEMA, hg.final]) -> hg.TS[int]:
            return values["value"]


def test_variadic_tsl_accepts_a_qualified_item_type():
    @hg.source_node
    def final_value() -> hg.TS[int, hg.final]:
        return 13

    @hg.graph
    def consume(*values: hg.TSL[hg.TS[int, hg.final], hg.SIZE]) -> hg.TS[int]:
        return values[0]

    @hg.graph
    def graph() -> hg.TS[int]:
        return consume(final_value())

    assert hg.run_graph(graph) == [13]


def test_variadic_tsl_retains_a_generic_bundle_item_annotation():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.compute_node
    def make_pair(value: hg.TS[int]) -> hg.TSB[Pair]:
        return {"left": value.value, "right": value.value * 10}

    @hg.graph
    def select_second(*values: hg.TSL[hg.TSB[hg.TS_SCHEMA], hg.SIZE]) -> hg.TS[int]:
        return hg.operators.getattr(values[1], hg.symbol("right"))

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        pair = make_pair(value)
        return select_second(pair, pair)

    assert hg.run_graph(graph, [1, 3]) == [10, 30]


def test_run_graph_var_args_can_capture_a_heterogeneous_tsb():
    @hg.compute_node
    def use_options(value: hg.TS[int], amount: int, factor: float) -> hg.TS[int]:
        return int((value.value + amount) * factor)

    @hg.graph
    def apply(*arguments: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return use_options(arguments[0], arguments[1], arguments[2])

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return apply(value, 2, 3.0)

    assert hg.run_graph(graph, [1, 2]) == [9, 12]


def test_node_receives_materialized_variadic_collections():
    @hg.compute_node
    def total(
        first: hg.TS[int],
        *values: hg.TSL[hg.TS[int], hg.SIZE],
        offset: hg.TS[int],
        **more: hg.TSB[hg.TS_SCHEMA],
    ) -> hg.TS[int]:
        return first.value + sum(values.value) + offset.value + sum(more.value.values())

    @hg.graph
    def graph(first: hg.TS[int], second: hg.TS[int], offset: hg.TS[int]) -> hg.TS[int]:
        return total(first, second, offset=offset, bonus=5)

    assert hg.run_graph(graph, [1, 2], [10, 20], [100, 200]) == [116, 227]


def test_positional_and_named_tsb_packs_resolve_independently():
    @hg.compute_node
    def total(
        *positional: hg.TSB[hg.TS_SCHEMA],
        **named: hg.TSB[hg.TS_SCHEMA],
    ) -> hg.TS[int]:
        return sum(positional.value.values()) + sum(named.value.values())

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return total(value, 2, amount=3)

    assert hg.run_graph(graph, [1, 4]) == [6, 9]


def test_empty_variadic_collections_tick_at_graph_start():
    @hg.compute_node
    def count_list(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return len(values.value)

    @hg.compute_node
    def count_bundle(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return len(values.value)

    @hg.graph
    def graph() -> hg.TS[int]:
        return count_list() + count_bundle()

    assert hg.run_graph(graph) == [0]


def test_variadic_tsb_can_be_forwarded_through_a_nested_graph():
    @hg.compute_node
    def total(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return sum(values.value.values())

    @hg.graph
    def forward(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return total(values=values)

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[int]) -> hg.TS[int]:
        return forward(left=left, right=right)

    assert hg.run_graph(graph, [1, 2], [10, 20]) == [11, 22]


def test_positional_and_named_tsb_packs_can_be_forwarded_together():
    @hg.compute_node
    def total(
        *inputs: hg.TSB[hg.TS_SCHEMA],
        **options: hg.TSB[hg.TS_SCHEMA],
    ) -> hg.TS[int]:
        return sum(inputs.value.values()) + sum(options.value.values())

    @hg.graph
    def forward(
        *inputs: hg.TSB[hg.TS_SCHEMA],
        **options: hg.TSB[hg.TS_SCHEMA],
    ) -> hg.TS[int]:
        return total(inputs=inputs, options=options)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return forward(value, 2, bonus=3)

    assert hg.run_graph(graph, [1, 4]) == [6, 9]


def test_exact_pack_name_accepts_a_prebuilt_tsb():
    @hg.schema
    class Pair:
        left: hg.TS[int]
        right: hg.TS[int]

    @hg.compute_node
    def make_pair(value: hg.TS[int]) -> hg.TSB[Pair]:
        return {"left": value.value, "right": value.value * 10}

    @hg.graph
    def select_right(**bundle: hg.TSB[Pair]) -> hg.TS[int]:
        return bundle["right"]

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return select_right(bundle=make_pair(value))

    assert hg.run_graph(graph, [1, 3]) == [10, 30]


def test_exact_pack_name_accepts_a_prebuilt_tsl():
    @hg.compute_node
    def make_list(value: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return {0: value.value, 1: value.value * 10}

    @hg.graph
    def select_second(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return values[1]

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return select_second(values=make_list(value))

    assert hg.run_graph(graph, [1, 3]) == [10, 30]


def test_map_can_invoke_a_variadic_python_graph():
    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value, 2: trigger.value * 10}

    @hg.graph
    def branch(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return values["value"]

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[int]:
        return hg.map(branch, value=make_values(trigger))[2]

    assert hg.run_graph(graph, [1, 3]) == [10, 30]


def test_variadic_sink_node_receives_updates():
    observed = []

    @hg.compute_node
    def record(**values: hg.TSB[hg.TS_SCHEMA]) -> None:
        observed.append(dict(values.value))

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        assert record(value=value, amount=2) is None
        return value

    assert hg.run_graph(graph, [1, 3]) == [1, 3]
    assert observed == [{"value": 1, "amount": 2}, {"value": 3, "amount": 2}]


def test_variadic_definition_preserves_its_python_signature():
    @hg.graph
    def definition(
        first: hg.TS[int],
        *values: hg.TSL[hg.TS[int], hg.SIZE],
        offset: int = 1,
        **options: hg.TSB[hg.TS_SCHEMA],
    ) -> hg.TS[int]:
        return first

    parameters = inspect.signature(definition).parameters

    assert parameters["values"].kind is inspect.Parameter.VAR_POSITIONAL
    assert parameters["offset"].kind is inspect.Parameter.KEYWORD_ONLY
    assert parameters["options"].kind is inspect.Parameter.VAR_KEYWORD


def test_fixed_keyword_only_parameters_are_passed_to_python_callbacks():
    @hg.compute_node
    def add_offset(value: hg.TS[int], *, offset: hg.TS[int]) -> hg.TS[int]:
        return value.value + offset.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return add_offset(value, offset=2)

    assert hg.run_graph(graph, [1, 3]) == [3, 5]


def test_variadic_tsl_rejects_scalars_and_mixed_time_series_types():
    @hg.compute_node
    def consume(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def scalar_graph(value: hg.TS[int]) -> hg.TS[int]:
        return consume(value, 2)

    @hg.graph
    def mixed_graph(integer: hg.TS[int], floating: hg.TS[float]) -> hg.TS[int]:
        return consume(integer, floating)

    with pytest.raises(ValueError, match="variadic TSL accepts only time series") as scalar_error:
        hg.run_graph(scalar_graph, [1])
    assert any("callback scalar_graph at scalar_graph[1]" in note for note in scalar_error.value.__notes__)
    with pytest.raises(ValueError, match="compatible item types"):
        hg.run_graph(mixed_graph, [1], [2.0])


def test_variadic_parameters_require_collection_annotations():
    with pytest.raises(ValueError, match="requires a time-series collection"):
        @hg.graph
        def scalar_args(*values: int) -> hg.TS[int]:
            raise AssertionError

    with pytest.raises(ValueError, match="TSL parameter requires var_args"):
        @hg.graph
        def list_kwargs(**values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
            raise AssertionError


def test_required_variadic_graph_captures_reject_optional_time_series_arguments():
    @hg.graph
    def capture_positional(*values: hg.TSL[hg.TS[int], hg.SIZE]) -> hg.TS[int]:
        return values[0]

    @hg.graph
    def capture_named(**values: hg.TSB[hg.TS_SCHEMA]) -> hg.TS[int]:
        return values["value"]

    @hg.graph
    def forward_positional(value: hg.TS[int]) -> hg.TS[int]:
        return capture_positional(hg.opt(value))

    @hg.graph
    def forward_named(value: hg.TS[int]) -> hg.TS[int]:
        return capture_named(value=hg.opt(value))

    diagnostic = "an opt-qualified argument requires an optional input parameter"
    with pytest.raises(ValueError, match=diagnostic):
        hg.run_graph(forward_positional, [1])
    with pytest.raises(ValueError, match=diagnostic):
        hg.run_graph(forward_named, [1])

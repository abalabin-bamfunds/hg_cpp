import math

import pytest

import hg


def test_diff_wraps_integer_subtraction_at_i64_width():
    @hg.graph
    def diff(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.diff(value)

    assert hg.run_graph(diff, [2**63 - 1, -(2**63)]) == [None, 1]


def test_diff_tracks_previous_valid_numeric_tick():
    @hg.graph
    def diff_integer(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.diff(value)

    @hg.graph
    def diff_double(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.diff(value)

    assert hg.run_graph(diff_integer, [10, 15, None, 15, hg.INVALID, None, 30, 37]) == [
        None,
        5,
        None,
        0,
        None,
        None,
        None,
        7,
    ]
    assert hg.run_graph(diff_double, [1.0, 2.5, 4.0]) == [None, 1.5, 1.5]


def test_clip_accepts_scalar_and_time_series_bounds():
    @hg.graph
    def scalar_clip() -> hg.TS[int]:
        return hg.operators.clip(5, 0, 10)

    @hg.graph
    def clip_integer(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.clip(value, 0, 10)

    @hg.graph
    def clip_double(value: hg.TS[float], minimum: hg.TS[float], maximum: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.clip(value, minimum, maximum)

    assert hg.run_graph(scalar_clip) == [5]
    assert hg.run_graph(clip_integer, [-5, 5, 20]) == [0, 5, 10]
    assert hg.run_graph(clip_double, [5.0, 5.0, 5.0], [0.0, 6.0, 0.0], [10.0, 10.0, 4.0]) == [
        5.0,
        6.0,
        4.0,
    ]


def test_clip_recomputes_for_maximum_only_and_scalar_value_bound_ticks():
    @hg.graph
    def clip_max(value: hg.TS[int], maximum: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.clip(value, 0, maximum)

    @hg.graph
    def clip_scalar_value(minimum: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.clip(5, minimum, 10)

    assert hg.run_graph(clip_max, [5, None, None], [10, 4, 8]) == [5, 4, 5]
    assert hg.run_graph(clip_scalar_value, [0, 6, 3]) == [5, 6, 5]


def test_clip_preserves_deduplicated_output():
    @hg.graph
    def clip(value: hg.TS[int]) -> hg.TS[int, hg.deduplicated]:
        return hg.operators.clip(hg.operators.dedup(value), 2, 8)

    assert hg.run_graph(clip, [1, 2, 3]) == [2, None, 3]


def test_clip_preserves_final_guarantee_when_all_inputs_are_final():
    @hg.source_node
    def final_value(value: hg.Scalar[int]) -> hg.TS[int, hg.final]:
        return value

    @hg.graph
    def clip() -> hg.TS[int, hg.final]:
        value = final_value(8)
        minimum = final_value(2)
        maximum = final_value(6)
        return hg.operators.clip(value, minimum, maximum)

    assert hg.run_graph(clip) == [6]


def test_clip_clamps_infinite_bounds_and_values():
    @hg.graph
    def clip(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.clip(value, 0.0, 10.0)

    assert hg.run_graph(clip, [-math.inf, math.inf]) == [0.0, 10.0]


def test_ewma_applies_warmup_to_valid_ticks_and_restarts_after_invalidation():
    @hg.graph
    def ewma(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.ewma(value, 0.5, 2)

    @hg.graph
    def ewma_valid(value: hg.TS[float]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.ewma(value, 0.5, 1))

    assert hg.run_graph(ewma, [1.0, None, 3.0, 5.0, 9.0]) == [None, None, None, 3.5, 6.25]
    assert hg.run_graph(ewma, [1.0, hg.INVALID, None, 10.0, 14.0, 18.0]) == [
        None,
        None,
        None,
        None,
        None,
        15.0,
    ]
    assert hg.run_graph(ewma_valid, [1.0, 3.0, hg.INVALID, None, 10.0, 14.0]) == [
        False,
        True,
        False,
        None,
        None,
        True,
    ]


@pytest.mark.parametrize(
    "alpha, min_periods",
    [(-0.1, 0), (1.1, 0), (math.inf, 0), (math.nan, 0), (0.5, -1)],
)
def test_ewma_rejects_invalid_scalar_configuration(alpha, min_periods):
    @hg.graph
    def ewma(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.ewma(value, alpha, min_periods)

    with pytest.raises(ValueError, match="cannot wire operator 'ewma'"):
        hg.run_graph(ewma, [1.0])

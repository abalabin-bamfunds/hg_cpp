import math

import pytest

import bg


def test_diff_wraps_integer_subtraction_at_i64_width():
    @bg.graph
    def diff(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.diff(value)

    assert bg.run_graph(diff, [2**63 - 1, -(2**63)]) == [None, 1]


def test_diff_tracks_previous_valid_numeric_tick():
    @bg.graph
    def diff_integer(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.diff(value)

    @bg.graph
    def diff_double(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.diff(value)

    assert bg.run_graph(diff_integer, [10, 15, None, 15, bg.INVALID, None, 30, 37]) == [
        None,
        5,
        None,
        0,
        None,
        None,
        None,
        7,
    ]
    assert bg.run_graph(diff_double, [1.0, 2.5, 4.0]) == [None, 1.5, 1.5]


def test_clip_accepts_scalar_and_time_series_bounds():
    @bg.graph
    def scalar_clip() -> bg.TS[int]:
        return bg.operators.clip(5, 0, 10)

    @bg.graph
    def clip_integer(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.clip(value, 0, 10)

    @bg.graph
    def clip_double(value: bg.TS[float], minimum: bg.TS[float], maximum: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.clip(value, minimum, maximum)

    assert bg.run_graph(scalar_clip) == [5]
    assert bg.run_graph(clip_integer, [-5, 5, 20]) == [0, 5, 10]
    assert bg.run_graph(clip_double, [5.0, 5.0, 5.0], [0.0, 6.0, 0.0], [10.0, 10.0, 4.0]) == [
        5.0,
        6.0,
        4.0,
    ]


def test_clip_recomputes_for_maximum_only_and_scalar_value_bound_ticks():
    @bg.graph
    def clip_max(value: bg.TS[int], maximum: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.clip(value, 0, maximum)

    @bg.graph
    def clip_scalar_value(minimum: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.clip(5, minimum, 10)

    assert bg.run_graph(clip_max, [5, None, None], [10, 4, 8]) == [5, 4, 5]
    assert bg.run_graph(clip_scalar_value, [0, 6, 3]) == [5, 6, 5]


def test_clip_preserves_deduplicated_output():
    @bg.graph
    def clip(value: bg.TS[int, bg.deduplicated]) -> bg.TS[int, bg.deduplicated]:
        return bg.operators.clip(value, 2, 8)

    assert bg.run_graph(clip, [1, 2, 3]) == [2, None, 3]


def test_clip_preserves_final_guarantee_when_all_inputs_are_final():
    @bg.graph
    def clip(
        value: bg.TS[int, bg.final],
        minimum: bg.TS[int, bg.final],
        maximum: bg.TS[int, bg.final],
    ) -> bg.TS[int, bg.final]:
        return bg.operators.clip(value, minimum, maximum)

    assert bg.run_graph(clip, [8], [2], [6]) == [6]


def test_clip_clamps_infinite_bounds_and_values():
    @bg.graph
    def clip(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.clip(value, 0.0, 10.0)

    assert bg.run_graph(clip, [-math.inf, math.inf]) == [0.0, 10.0]


def test_ewma_applies_warmup_to_valid_ticks_and_restarts_after_invalidation():
    @bg.graph
    def ewma(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.ewma(value, 0.5, 2)

    @bg.graph
    def ewma_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.ewma(value, 0.5, 1))

    assert bg.run_graph(ewma, [1.0, None, 3.0, 5.0, 9.0]) == [None, None, None, 3.5, 6.25]
    assert bg.run_graph(ewma, [1.0, bg.INVALID, None, 10.0, 14.0, 18.0]) == [
        None,
        None,
        None,
        None,
        None,
        15.0,
    ]
    assert bg.run_graph(ewma_valid, [1.0, 3.0, bg.INVALID, None, 10.0, 14.0]) == [
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
    @bg.graph
    def ewma(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.ewma(value, alpha, min_periods)

    with pytest.raises(ValueError, match="cannot wire operator 'ewma'"):
        bg.run_graph(ewma, [1.0])

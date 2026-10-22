import math

import pytest

import bg


def test_diff_tracks_previous_valid_numeric_tick():
    @bg.graph
    def diff_integer(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.diff(value)

    @bg.graph
    def diff_double(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.diff(value)

    assert bg.test_graph(diff_integer, [10, 15, None, 15, bg.INVALID, None, 30, 37]) == [
        None,
        5,
        None,
        0,
        None,
        None,
        None,
        7,
    ]
    assert bg.test_graph(diff_double, [1.0, 2.5, 4.0]) == [None, 1.5, 1.5]


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

    assert bg.test_graph(scalar_clip) == [5]
    assert bg.test_graph(clip_integer, [-5, 5, 20]) == [0, 5, 10]
    assert bg.test_graph(clip_double, [5.0, 5.0, 5.0], [0.0, 6.0, 0.0], [10.0, 10.0, 4.0]) == [
        5.0,
        6.0,
        4.0,
    ]


def test_ewma_applies_warmup_to_valid_ticks_and_restarts_after_invalidation():
    @bg.graph
    def ewma(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.ewma(value, 0.5, 2)

    @bg.graph
    def ewma_valid(value: bg.TS[float]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.ewma(value, 0.5, 1))

    assert bg.test_graph(ewma, [1.0, None, 3.0, 5.0, 9.0]) == [None, None, None, 3.5, 6.25]
    assert bg.test_graph(ewma, [1.0, bg.INVALID, None, 10.0, 14.0, 18.0]) == [
        None,
        None,
        None,
        None,
        None,
        15.0,
    ]
    assert bg.test_graph(ewma_valid, [1.0, 3.0, bg.INVALID, None, 10.0, 14.0]) == [
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
        bg.test_graph(ewma, [1.0])

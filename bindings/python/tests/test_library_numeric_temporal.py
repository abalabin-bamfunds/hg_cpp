from datetime import datetime, timedelta
import math

import hgraph as hg
import pytest


def test_divmod_operator_returns_quotient_and_remainder_for_integer_and_double_series():
    @hg.graph
    def integer_divmod(lhs: hg.TS[int], rhs: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return divmod(lhs, rhs)

    @hg.graph
    def double_divmod(lhs: hg.TS[float], rhs: hg.TS[float]) -> hg.TSL[hg.TS[float], 2]:
        return divmod(lhs, rhs)

    assert hg.run_graph(integer_divmod, [-7, 7], [3, -3]) == [{0: -3, 1: 2}, {0: -3, 1: -2}]
    assert hg.run_graph(double_divmod, [-7.0, 7.0], [3.0, -3.0]) == [{0: -3.0, 1: 2.0}, {0: -3.0, 1: -2.0}]
    assert hg.run_graph(integer_divmod, [7, hg.INVALID, 9], [3, None, 4]) == [
        {0: 2, 1: 1},
        None,
        {0: 2, 1: 1},
    ]


def test_divmod_operator_promotes_scalar_operands():
    @hg.graph
    def scalar_rhs(value: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return hg.operators.divmod(value, 3)

    @hg.graph
    def scalar_lhs(value: hg.TS[int]) -> hg.TSL[hg.TS[int], 2]:
        return hg.operators.divmod(7, value)

    @hg.graph
    def scalar_operands() -> hg.TSL[hg.TS[int], 2]:
        return hg.operators.divmod(7, 3)

    assert hg.run_graph(scalar_rhs, [7, -7]) == [{0: 2, 1: 1}, {0: -3, 1: 2}]
    assert hg.run_graph(scalar_lhs, [3, -3]) == [{0: 2, 1: 1}, {0: -3, 1: -2}]
    assert hg.run_graph(scalar_operands) == [{0: 2, 1: 1}]


def test_shift_operators_support_scalar_and_time_series_operands():
    @hg.graph
    def left_shift(value: hg.TS[int]) -> hg.TS[int]:
        return value << 2

    @hg.graph
    def right_shift(value: hg.TS[int]) -> hg.TS[int]:
        return value >> 1

    @hg.graph
    def scalar_left_shift(value: hg.TS[int]) -> hg.TS[int]:
        return 1 << value

    assert hg.run_graph(left_shift, [1, -1]) == [4, -4]
    assert hg.run_graph(right_shift, [-7, 7]) == [-4, 3]
    assert hg.run_graph(scalar_left_shift, [2, 3]) == [4, 8]


def test_ln_and_sign_follow_numeric_scalar_semantics():
    @hg.graph
    def natural_log(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.ln(value)

    @hg.graph
    def sign(value: hg.TS[float]) -> hg.TS[float]:
        return hg.operators.sign(value)

    assert hg.run_graph(natural_log, [1.0, math.e]) == [0.0, 1.0]
    result = hg.run_graph(sign, [-2.0, -0.0, 0.0, 3.0])
    assert result[:3] == [-1.0, 0.0, 0.0]
    assert not math.copysign(1.0, result[1]) < 0
    assert not math.copysign(1.0, result[2]) < 0
    assert hg.run_graph(sign, [float("-inf"), float("inf")]) == [-1.0, 1.0]
    assert math.isnan(hg.run_graph(sign, [float("nan")])[0])
    assert math.isnan(hg.run_graph(natural_log, [float("nan")])[0])
    assert math.isinf(hg.run_graph(natural_log, [float("inf")])[0])
    with pytest.raises(ValueError, match="natural logarithm requires a positive value"):
        hg.run_graph(natural_log, [-1.0])


def test_duration_arithmetic_supports_reverse_scaling_and_time_series_ratios():
    @hg.graph
    def multiply(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return value * 0.5

    @hg.graph
    def multiply_reversed(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return 3 * value

    @hg.graph
    def multiply_fractional_reversed(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return 0.5 * value

    @hg.graph
    def divide_integer(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return value / 2

    @hg.graph
    def divide_infinity(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return value / math.inf

    @hg.graph
    def divide_nan(value: hg.TS[timedelta]) -> hg.TS[timedelta]:
        return value / math.nan

    @hg.graph
    def duration_ratio(lhs: hg.TS[timedelta], rhs: hg.TS[timedelta]) -> hg.TS[float]:
        return lhs / rhs

    @hg.graph
    def floor_duration_ratio(lhs: hg.TS[timedelta], rhs: hg.TS[timedelta]) -> hg.TS[int]:
        return lhs // rhs

    fractional_values = [timedelta(microseconds=value) for value in (1, 3, -3)]
    rounded = [timedelta(0), timedelta(microseconds=2), timedelta(microseconds=-2)]
    assert hg.run_graph(multiply, fractional_values) == rounded
    assert hg.run_graph(multiply_fractional_reversed, fractional_values) == rounded
    assert hg.run_graph(divide_integer, fractional_values) == rounded
    with pytest.raises(ValueError, match="duration divisor must be finite"):
        hg.run_graph(divide_infinity, [timedelta(microseconds=1)])
    with pytest.raises(ValueError, match="duration divisor must be finite"):
        hg.run_graph(divide_nan, [timedelta(microseconds=1)])
    assert hg.run_graph(multiply_reversed, [timedelta(microseconds=1), timedelta(microseconds=-2)]) == [
        timedelta(microseconds=3),
        timedelta(microseconds=-6),
    ]
    assert hg.run_graph(
        duration_ratio,
        [timedelta(microseconds=-7), timedelta(microseconds=7)],
        [timedelta(microseconds=3), timedelta(microseconds=-3)],
    ) == [-7 / 3, 7 / -3]
    assert hg.run_graph(
        floor_duration_ratio,
        [timedelta(microseconds=-7), timedelta(microseconds=7)],
        [timedelta(microseconds=3), timedelta(microseconds=-3)],
    ) == [-3, -3]


def test_date_time_and_timedelta_attributes_are_available_to_python_graphs():
    @hg.graph
    def date_fields(value: hg.TS[datetime]) -> hg.TS[int]:
        return hg.operators.getattr(value, hg.symbol("year"))

    @hg.graph
    def duration_seconds(value: hg.TS[timedelta]) -> hg.TS[float]:
        return hg.operators.getattr(value, hg.symbol("total_seconds"))

    @hg.graph
    def missing_date_field(value: hg.TS[datetime]) -> hg.TS[int]:
        return hg.operators.getattr(value, hg.symbol("timezone"))

    @hg.graph
    def missing_duration_field(value: hg.TS[timedelta]) -> hg.TS[int]:
        return hg.operators.getattr(value, hg.symbol("weeks"))

    assert hg.run_graph(date_fields, [datetime(2024, 2, 29), None, datetime(1969, 12, 31)]) == [
        2024,
        None,
        1969,
    ]
    assert hg.run_graph(duration_seconds, [timedelta(microseconds=-1), timedelta(days=1, microseconds=1)]) == [
        -0.000001,
        86400.000001,
    ]
    with pytest.raises(ValueError):
        hg.run_graph(missing_date_field, [datetime(2024, 1, 1)])
    with pytest.raises(ValueError):
        hg.run_graph(missing_duration_field, [timedelta(days=1)])

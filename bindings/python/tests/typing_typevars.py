"""Typing syntax smoke fixture for generic Python graph annotations."""

from typing import TypeVar

import bg


VALUE = TypeVar("VALUE")
SERIES = TypeVar("SERIES", bound=bg.TimeSeries)


@bg.graph
def scalar_identity(value: bg.TS[VALUE]) -> bg.TS[VALUE]:
    return value


@bg.graph
def series_identity(value: SERIES) -> SERIES:
    return value


def use_scalar_identity(value: bg.TS[int]) -> bg.TS[int]:
    return scalar_identity(value)


def use_series_identity(value: bg.TS[int]) -> bg.TS[int]:
    return series_identity(value)

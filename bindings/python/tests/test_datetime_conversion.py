from datetime import datetime, timedelta
import os
import subprocess
import sys

import hg
import pytest


EPOCH = datetime(1970, 1, 1)


@pytest.mark.parametrize(
    "value",
    [
        datetime.min,
        datetime(1900, 2, 28, 23, 59, 59, 123456),
        EPOCH - timedelta(microseconds=1),
        EPOCH,
        EPOCH + timedelta(microseconds=1),
        datetime(2024, 2, 29, 12, 34, 56, 123456),
        datetime.max,
    ],
)
def test_native_datetime_binding_uses_exact_utc_microseconds(value):
    extension = pytest.importorskip("hg._hg_test_extension")
    elapsed = value - EPOCH
    expected = (elapsed.days * 86400 + elapsed.seconds) * 1_000_000 + elapsed.microseconds

    assert extension.evaluation_time_microseconds(value) == expected
    assert extension.evaluation_time_from_microseconds(expected) == value


@pytest.mark.parametrize("count", [-(1 << 63), -62135596800000001, 253402300800000000, (1 << 63) - 1])
def test_native_datetime_binding_rejects_times_outside_python_calendar(count):
    extension = pytest.importorskip("hg._hg_test_extension")

    with pytest.raises(OverflowError, match="outside the datetime.datetime range"):
        extension.evaluation_time_from_microseconds(count)


def test_generator_absolute_times_use_utc_microseconds():
    start = EPOCH + timedelta(microseconds=1)

    @hg.generator
    def source() -> hg.TS[int]:
        yield start, 1
        yield start + timedelta(microseconds=1), 2

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    result = hg.run_graph(graph, start_time=start, stop_time=start + timedelta(microseconds=2))

    assert result == [1, 2]
    assert result.start_time == start
    assert result.events == ((start, 1), (start + timedelta(microseconds=1), 2))


@pytest.mark.parametrize("zone", ["EST5", "JST-9"])
def test_datetime_conversion_and_clock_scheduling_ignore_process_timezone(zone):
    pytest.importorskip("hg._hg_test_extension")
    environment = os.environ.copy()
    environment["TZ"] = zone
    source = r'''
from datetime import datetime, timedelta
import time
import hg
from hg import _hg_test_extension as extension

if hasattr(time, "tzset"):
    time.tzset()
epoch = datetime(1970, 1, 1)
assert datetime.fromtimestamp(0) != epoch
assert extension.evaluation_time_microseconds(epoch) == 0
assert extension.evaluation_time_from_microseconds(-1) == epoch - timedelta(microseconds=1)
assert extension.date_time_conversions_can_be_registered()[1] == epoch
observed = []
start = epoch + timedelta(microseconds=1)

@hg.node
def source(clock: hg.EvaluationClock = None, scheduler: hg.Scheduler = None) -> datetime:
    observed.append(clock.evaluation_time)
    assert clock.now == clock.evaluation_time
    if len(observed) == 1:
        scheduler.schedule_at(start + timedelta(microseconds=1))
    return clock.evaluation_time

@hg.graph
def graph() -> hg.TS[datetime]:
    return source()

result = hg.run_graph(graph, start_time=start, stop_time=start + timedelta(microseconds=2))
expected = [start, start + timedelta(microseconds=1)]
assert observed == expected
assert result == expected
assert result.start_time == start
assert result.events == tuple(zip(expected, expected))
observed.clear()
assert hg.run_graph(graph) == expected
assert observed == expected
'''
    result = subprocess.run(
        [sys.executable, "-c", source], env=environment, capture_output=True, text=True, timeout=15
    )

    assert result.returncode == 0, result.stdout + result.stderr

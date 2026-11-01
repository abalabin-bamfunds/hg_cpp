import gc
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import weakref
from datetime import datetime, timedelta, timezone

import hg
import pytest
from hg import _hg


def test_imports_the_expected_package():
    expected_package = os.environ.get("HG_PYTHON_PACKAGE_DIR")
    if expected_package is None:
        pytest.skip("HG_PYTHON_PACKAGE_DIR is not set")

    package_dir = Path(expected_package).resolve()
    assert Path(hg.__file__).resolve().parent == package_dir

    extension = importlib.util.find_spec("hg._hg")
    assert extension is not None
    assert Path(extension.origin).resolve().parent == package_dir


def test_run_graph_wires_python_nodes_and_runs_each_input_tick():
    @hg.node
    def increment(value: int) -> int:
        return value + 1

    @hg.graph
    def graph(value: int) -> int:
        return increment(value)

    assert hg.run_graph(graph, 41) == [42]
    assert hg.run_graph(graph, [1, 2, None, 4]) == [2, 3, None, 5]


def test_node_decorator_preserves_function_metadata():
    @hg.compute_node
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        """Return the supplied value."""
        return value.value

    assert identity.__name__ == "identity"
    assert identity.__doc__ == "Return the supplied value."
    assert identity.__wrapped__ is identity.function


def test_run_graph_composes_nested_graphs_and_scalar_arguments():
    @hg.node
    def add(value: int, offset: int) -> int:
        return value + offset

    @hg.graph
    def add_default(value: int, offset: int = 3) -> int:
        return add(value, offset=offset)

    @hg.graph
    def graph(value: int) -> int:
        return add_default(value)

    assert hg.run_graph(graph, [1, 2]) == [4, 5]


def test_run_graph_wiring_retains_a_locally_declared_node_until_execution():
    @hg.graph
    def graph(value: int) -> int:
        @hg.node
        def increment(inner: int) -> int:
            return inner + 1

        return increment(value)

    assert hg.run_graph(graph, [2, 4]) == [3, 5]


def test_python_source_node_runs_at_graph_start():
    @hg.source_node
    def source() -> hg.TS[int]:
        return 7

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [7]


def test_python_time_series_input_accepts_a_constant():
    @hg.compute_node
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph() -> hg.TS[int]:
        return identity(7)

    assert hg.run_graph(graph) == [7]


def test_python_node_receives_optional_scalar_as_none():
    @hg.compute_node
    def add_optional(value: hg.TS[int], offset: int | None = None) -> hg.TS[int]:
        return value.value + (offset or 0)

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return add_optional(value, offset=None)

    assert hg.run_graph(graph, [7]) == [7]


@pytest.mark.parametrize(
    ("annotation", "value"),
    [
        (bool, True),
        (float, 2.5),
        (hg.Symbol, hg.symbol("market")),
    ],
)
def test_source_node_scalar_outputs(annotation, value):
    @hg.source_node
    def source() -> hg.TS[annotation]:
        return value

    @hg.graph
    def graph() -> hg.TS[annotation]:
        return source()

    assert hg.run_graph(graph) == [value]


def test_extension_modules_share_python_conversion_registry():
    extension = pytest.importorskip("hg._hg_test_extension")

    assert _hg._scalar_conversions_address() == extension.scalar_conversions_address()


def test_runtime_scalar_conversion_overrides_semantic_type_conversion():
    extension = pytest.importorskip("hg._hg_test_extension")

    added, value, view_added, view = extension.runtime_conversion_overrides_type_conversion()

    assert added
    assert value == "runtime double conversion"
    assert view_added
    assert view == "runtime double view conversion"


def test_date_time_scalar_conversion_is_builtin_and_extensions_cannot_override_it():
    extension = pytest.importorskip("hg._hg_test_extension")

    date_time_added, date_time, timedelta_added, duration, date_time_view = (
        extension.date_time_conversions_can_be_registered()
    )
    assert not date_time_added
    assert date_time == datetime.fromtimestamp(0)
    assert not timedelta_added
    assert duration == timedelta(0)
    assert date_time_view == datetime.fromtimestamp(0)


@pytest.mark.parametrize(
    "value",
    [
        datetime(1970, 1, 1),
        datetime(1969, 12, 31, 23, 59, 59, 999999),
        datetime(2024, 5, 6, 7, 8, 9, 123456),
    ],
)
def test_date_time_scalar_conversion_round_trips_microseconds(value):
    @hg.graph
    def constant() -> hg.TS[datetime]:
        return hg.operators.const_(value)

    @hg.graph
    def identity(value: hg.TS[datetime]) -> hg.TS[datetime]:
        return value

    assert hg.run_graph(constant) == [value]
    assert hg.run_graph(identity, [value]) == [value]


def test_date_time_scalar_conversion_supports_nested_scalar_values():
    values = (datetime(1969, 12, 31, 23, 59, 59, 999999), datetime(2024, 5, 6, 7, 8, 9, 123456))

    @hg.graph
    def identity(value: hg.TS[tuple[datetime, ...]]) -> hg.TS[tuple[datetime, ...]]:
        return value

    assert hg.run_graph(identity, [values]) == [values]


def test_date_time_scalar_conversion_supports_temporal_operators():
    value = datetime(2024, 5, 6, 7, 8, 9, 123456)

    @hg.graph
    def shifted(value: hg.TS[datetime]) -> hg.TS[datetime]:
        return value + timedelta(microseconds=1)

    @hg.graph
    def difference(left: hg.TS[datetime], right: hg.TS[datetime]) -> hg.TS[timedelta]:
        return left - right

    assert hg.run_graph(shifted, [value]) == [value + timedelta(microseconds=1)]
    assert hg.run_graph(difference, [value + timedelta(microseconds=2)], [value]) == [
        timedelta(microseconds=2)
    ]


@pytest.mark.parametrize(
    "value",
    [
        datetime(2024, 5, 6, tzinfo=timezone.utc),
        datetime(2024, 5, 6, tzinfo=timezone(timedelta(hours=2))),
    ],
)
def test_date_time_scalar_conversion_rejects_timezone_aware_values(value):
    @hg.graph
    def identity(value: hg.TS[datetime]) -> hg.TS[datetime]:
        return value

    with pytest.raises(TypeError, match="timezone-naive datetime"):
        hg.run_graph(identity, [value])


@pytest.mark.parametrize("value", [datetime(2024, 5, 6).date(), datetime(2024, 5, 6).time()])
def test_date_time_scalar_conversion_rejects_date_and_time_values(value):
    @hg.graph
    def identity(value: hg.TS[datetime]) -> hg.TS[datetime]:
        return value

    with pytest.raises(TypeError, match="expected datetime.datetime"):
        hg.run_graph(identity, [value])


@pytest.mark.parametrize(
    "value",
    [
        timedelta(0),
        timedelta(microseconds=1),
        timedelta(microseconds=-1),
        timedelta(days=2, seconds=3, microseconds=4),
        timedelta(days=-2, seconds=3, microseconds=4),
        timedelta(microseconds=-(1 << 63)),
        timedelta(microseconds=(1 << 63) - 1),
    ],
)
def test_timedelta_scalar_conversion_round_trips_exact_microseconds(value):
    @hg.graph
    def constant() -> hg.TS[timedelta]:
        return hg.operators.const_(value)

    assert hg.run_graph(constant) == [value]


@pytest.mark.parametrize("microseconds", [-(1 << 63) - 1, 1 << 63])
def test_timedelta_scalar_conversion_rejects_values_outside_i64_microseconds(microseconds):
    value = timedelta(microseconds=microseconds)

    @hg.graph
    def constant() -> hg.TS[timedelta]:
        return hg.operators.const_(value)

    with pytest.raises(OverflowError, match="outside the hg.timedelta_t range"):
        hg.run_graph(constant)


def test_timedelta_conversion_accepts_subclasses_and_rejects_other_values():
    extension = pytest.importorskip("hg._hg_test_extension")

    class CustomTimedelta(timedelta):
        pass

    assert extension.convert_timedelta(CustomTimedelta(seconds=-2, microseconds=3)) == timedelta(
        seconds=-2, microseconds=3
    )
    with pytest.raises(TypeError, match="expected datetime.timedelta"):
        extension.convert_timedelta(3)


def test_timed_operators_accept_datetime_timedelta():
    @hg.graph
    def delayed(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.delay(value, timedelta(microseconds=2))

    @hg.graph
    def scheduled() -> hg.TS[bool]:
        return hg.operators.schedule(timedelta(microseconds=1), max_ticks=3)

    @hg.graph
    def throttled(value: hg.TS[int]) -> hg.TS[int]:
        return hg.operators.throttle(value, timedelta(microseconds=2))

    assert hg.run_graph(delayed, [1, 2, None, 4]) == [None, None, 1, 2, None, 4]
    assert hg.run_graph(scheduled) == [None, True, True, True]
    assert hg.run_graph(throttled, [1, 2, 3, 4]) == [1, None, 3, None, 4]


def test_nested_delay_releases_pending_python_values_when_stopped():
    class WeakString(str):
        pass

    value = WeakString("pending delayed value")
    value_reference = weakref.ref(value)

    @hg.graph
    def delayed(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.delay(value, timedelta(microseconds=10))

    @hg.graph
    def fallback(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.const_("fallback")

    @hg.graph
    def graph(key: hg.TS[int], value: hg.TS[str]) -> hg.TS[str]:
        return hg.switch_(key, {1: delayed, hg.DEFAULT: fallback}, value)

    result = hg.run_graph(graph, [1, 0], [value, None])
    assert result == [None, "fallback"]

    del result
    del value
    gc.collect()
    assert value_reference() is None


def test_run_graph_uses_registered_runtime_scalar_conversion():
    script = """
import hg
from hg._hg_test_extension import register_runtime_double_result_conversion

assert register_runtime_double_result_conversion()

@hg.graph
def graph(value: hg.TS[float]) -> hg.TS[float]:
    return value

assert hg.run_graph(graph, [2.5]) == ["extension scalar conversion"]

@hg.graph
def nested(values: hg.TSD[int, hg.TS[float]]) -> hg.TSD[int, hg.TS[float]]:
    return values

assert hg.run_graph(nested, [{1: 2.5}]) == [{1: "extension scalar conversion"}]
"""
    subprocess.run([sys.executable, "-c", script], check=True, capture_output=True, text=True)


def test_symbol_is_a_value_type():
    value = hg.symbol("price")

    assert isinstance(value, str)
    assert value == hg.Symbol("price")
    assert "price" == value
    assert not (value != "price")
    assert not ("price" != value)
    assert hash(value) == hash(hg.symbol("price"))
    assert type(str(value)) is str
    assert str(value) == "price"
    assert repr(value) == "symbol('price')"
    assert value.upper() == "PRICE"
    assert value.startswith("pr")

    values = {value: 1}
    assert values["price"] == 1
    assert values[hg.Symbol("price")] == 1


def test_null_symbol_is_distinct_from_empty_symbol():
    null_symbol = hg.Symbol()

    assert null_symbol == hg.Symbol()
    assert null_symbol != hg.symbol("")
    assert hg.symbol("") != null_symbol
    assert null_symbol != ""
    assert "" != null_symbol
    assert not (null_symbol == "")
    assert not ("" == null_symbol)
    values = {null_symbol: 1, "": 2}
    assert len(values) == 2
    assert values[null_symbol] == 1
    assert values[""] == 2
    assert str(null_symbol) == ""
    assert type(str(null_symbol)) is str
    assert repr(null_symbol) == "symbol()"


def test_symbol_scalar_inference_keeps_symbols_distinct_from_strings():
    symbol = hg.symbol("name")

    @hg.graph
    def symbol_graph() -> hg.TS[hg.Symbol]:
        return hg.operators.const_(symbol)

    @hg.graph
    def string_graph() -> hg.TS[str]:
        return hg.operators.const_("name")

    symbol_result = hg.run_graph(symbol_graph)
    string_result = hg.run_graph(string_graph)

    assert type(symbol_result[0]) is hg.Symbol
    assert symbol_result[0] is symbol
    assert type(string_result[0]) is str
    assert string_result[0] == "name"


def test_explicit_symbol_input_accepts_plain_python_text():
    @hg.compute_node
    def echo(trigger: hg.TS[int], label: hg.Symbol) -> hg.TS[hg.Symbol]:
        return label

    @hg.graph
    def graph(trigger: hg.TS[int]) -> hg.TS[hg.Symbol]:
        return echo(trigger, label="explicit symbol")

    result = hg.run_graph(graph, [1])

    assert type(result[0]) is hg.Symbol
    assert result[0] == "explicit symbol"


def test_symbol_time_series_input_accepts_plain_python_text():
    @hg.compute_node
    def identity(value: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return identity(value)

    result = hg.run_graph(graph, ["time series symbol"])

    assert type(result[0]) is hg.Symbol
    assert result[0] == "time series symbol"


def test_symbol_as_string_input_is_normalized_to_plain_text():
    @hg.compute_node
    def identity(value: hg.TS[str]) -> hg.TS[str]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[str]) -> hg.TS[str]:
        return identity(value)

    result = hg.run_graph(graph, [hg.Symbol("plain text")])

    assert result == ["plain text"]
    assert type(result[0]) is str


def test_symbol_round_trip_reuses_python_object_for_repeated_cpp_symbol():
    value = hg.symbol("repeated")

    @hg.compute_node
    def identity(item: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return item.value

    @hg.graph
    def graph(item: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return identity(item)

    result = hg.run_graph(graph, [value, value])

    assert result == [value, value]
    assert result[0] is value
    assert result[0] is result[1]


def test_null_symbol_round_trips_as_null():
    @hg.compute_node
    def identity(value: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[hg.Symbol]) -> hg.TS[hg.Symbol]:
        return identity(value)

    null_symbol = hg.Symbol()
    result = hg.run_graph(graph, [null_symbol])

    assert result[0] is null_symbol
    assert result[0]._is_null


def test_symbol_cannot_be_subclassed():
    with pytest.raises(TypeError, match="cannot be subclassed"):
        class NamedSymbol(hg.Symbol):
            pass

    symbol = hg.symbol("fixed")
    with pytest.raises(AttributeError, match="immutable"):
        symbol._is_null = True
    with pytest.raises(AttributeError, match="immutable"):
        del symbol._is_null


def test_python_string_and_symbol_scalar_arguments():
    @hg.source_node
    def source() -> hg.TS[int]:
        return 1

    @hg.compute_node
    def add_text(value: hg.TS[int], text: str, label: hg.Symbol) -> hg.TS[int]:
        return value.value + len(text) + len(str(label))

    @hg.graph
    def graph() -> hg.TS[int]:
        return add_text(source(), text="market", label=hg.symbol("open"))

    assert hg.run_graph(graph) == [1 + len("market") + len("open")]


def test_python_scalar_string_argument_outlives_wiring_allocator():
    text = "wiring-owned café\x00市場"

    @hg.source_node
    def source() -> hg.TS[int]:
        return 1

    @hg.compute_node
    def echo(value: hg.TS[int], text: str) -> hg.TS[str]:
        return text

    @hg.graph
    def graph() -> hg.TS[str]:
        return echo(source(), text)

    assert hg.run_graph(graph) == [text]


def test_python_string_time_series_source_input_and_output():
    text = "café\x00市場"

    @hg.source_node
    def source() -> hg.TS[str]:
        return text

    @hg.compute_node
    def append(value: hg.TS[str]) -> hg.TS[str]:
        return value.value + "\x00done"

    @hg.graph
    def source_graph() -> hg.TS[str]:
        return source()

    @hg.graph
    def input_graph(value: hg.TS[str]) -> hg.TS[str]:
        return append(value)

    source_result = hg.run_graph(source_graph)
    assert source_result == [text]
    assert source_result[0] is text
    assert hg.run_graph(input_graph, [text]) == [text + "\x00done"]


def test_python_string_time_series_round_trip_preserves_identity():
    text = "reused string"

    @hg.compute_node
    def identity(value: hg.TS[str]) -> hg.TS[str]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[str]) -> hg.TS[str]:
        return identity(value)

    result = hg.run_graph(graph, [text])

    assert result[0] is text


def test_python_string_cache_releases_values_with_hg_storage():
    class WeakString(str):
        pass

    text = WeakString("cache lifetime")
    text_reference = weakref.ref(text)

    @hg.compute_node
    def identity(value: hg.TS[str]) -> hg.TS[str]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[str]) -> hg.TS[str]:
        return identity(value)

    result = hg.run_graph(graph, [text])
    assert result[0] is text

    del result
    del text
    gc.collect()
    assert text_reference() is None


def test_python_string_rejects_lone_surrogates():
    @hg.compute_node
    def identity(value: hg.TS[str]) -> hg.TS[str]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[str]) -> hg.TS[str]:
        return identity(value)

    with pytest.raises(UnicodeEncodeError):
        hg.run_graph(graph, ["lone surrogate: \ud800"])


def test_python_string_rejects_invalid_hg_utf8():
    extension = pytest.importorskip("hg._hg_test_extension")

    with pytest.raises(UnicodeDecodeError):
        extension.decode_invalid_string()


def test_input_view_expires_after_the_callback():
    saved_inputs = []

    @hg.compute_node
    def save_input(value: hg.TS[int]) -> hg.TS[int]:
        saved_inputs.append(value)
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return save_input(value)

    assert hg.run_graph(graph, [7]) == [7]
    with pytest.raises(RuntimeError, match="outside its callback"):
        _ = saved_inputs[0].value


def test_input_view_from_an_earlier_callback_stays_expired():
    saved_inputs = []

    @hg.compute_node
    def save_input(value: hg.TS[int]) -> hg.TS[int]:
        if saved_inputs:
            with pytest.raises(RuntimeError, match="outside its callback"):
                _ = saved_inputs[-1].value
        saved_inputs.append(value)
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return save_input(value)

    assert hg.run_graph(graph, [7, 8]) == [7, 8]


def test_wiring_point_expires_after_graph_construction():
    saved_points = []

    @hg.compute_node
    def identity(value: hg.TS[int]) -> hg.TS[int]:
        return value.value

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        saved_points.append(value)
        return identity(value)

    assert hg.run_graph(graph, [7]) == [7]
    with pytest.raises(RuntimeError, match="outside its graph construction"):
        _ = saved_points[0].has_output


def test_repeated_node_wiring_is_deduplicated():
    evaluations = []

    @hg.compute_node
    def increment(value: hg.TS[int]) -> hg.TS[int]:
        evaluations.append(value.value)
        return value.value + 1

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        increment(value)
        return increment(value)

    assert hg.run_graph(graph, [3]) == [4]
    assert evaluations == [3]


def test_callback_exception_propagates_from_run_graph():
    @hg.compute_node
    def fail(value: hg.TS[int]) -> hg.TS[int]:
        raise ValueError("callback failed")

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return fail(value)

    with pytest.raises(ValueError, match="callback failed"):
        hg.run_graph(graph, [3])


def test_run_graph_callback_exception_keeps_identity_traceback_and_wiring_note():
    class CallbackError(Exception):
        pass

    callback_error = CallbackError("graph callback failed")

    @hg.graph
    def invalid() -> hg.TS[int]:
        raise callback_error

    with pytest.raises(CallbackError, match="graph callback failed") as raised:
        hg.run_graph(invalid)

    assert raised.value is callback_error
    traceback_names = []
    traceback = raised.value.__traceback__
    while traceback is not None:
        traceback_names.append(traceback.tb_frame.f_code.co_name)
        traceback = traceback.tb_next
    assert "invalid" in traceback_names
    assert any("callback invalid at invalid[0]" in note for note in raised.value.__notes__)
    assert all("Traceback (most recent call last)" not in note for note in raised.value.__notes__)


def test_none_requests_no_output_change():
    @hg.compute_node
    def no_change(value: hg.TS[int]) -> hg.TS[int]:
        return None

    @hg.graph
    def graph(value: hg.TS[int]) -> hg.TS[int]:
        return no_change(value)

    assert hg.run_graph(graph, [3]) == [None]


def test_sink_graph_runs_without_an_output():
    received = []

    @hg.compute_node
    def sink(value: hg.TS[int]) -> None:
        received.append(value.value)

    @hg.graph
    def graph(value: hg.TS[int]) -> None:
        sink(value)

    assert hg.run_graph(graph, [3, 5]) is None
    assert received == [3, 5]


def test_python_callback_receives_interleaved_time_series_and_scalars():
    @hg.compute_node
    def combine(left: hg.TS[int], offset: int, right: hg.TS[float], scale: float) -> hg.TS[float]:
        return (left.value + offset) * scale + right.value

    @hg.graph
    def graph(left: hg.TS[int], right: hg.TS[float]) -> hg.TS[float]:
        return combine(left, 2, right, 0.5)

    assert hg.run_graph(graph, [2, 4], [10.0, 20.0]) == [12.0, 23.0]


def test_module_scope_definitions_do_not_leak_native_instances_at_shutdown():
    script = """
import hg

@hg.compute_node
def identity(value: hg.TS[int]) -> hg.TS[int]:
    return value.value

@hg.graph
def graph(value: hg.TS[int]) -> hg.TS[int]:
    return identity(value)
"""

    completed = subprocess.run(
        [sys.executable, "-c", script],
        check=False,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0, completed.stderr
    assert "nanobind: leaked" not in completed.stderr


def test_run_graph_definition_retains_and_releases_python_callback():
    def make_graph():
        def graph() -> None:
            pass

        return graph, weakref.ref(graph)

    callback, callback_ref = make_graph()
    definition = hg.graph(callback)

    del callback
    gc.collect()
    assert callback_ref() is definition.function

    del definition
    gc.collect()
    assert callback_ref() is None


def test_node_definition_retains_and_releases_python_callback():
    def make_node():
        def identity(value: hg.TS[int]) -> hg.TS[int]:
            return value.value

        return identity, weakref.ref(identity)

    callback, callback_ref = make_node()
    definition = hg.compute_node(callback)

    del callback
    gc.collect()
    assert callback_ref() is definition.function

    del definition
    gc.collect()
    assert callback_ref() is None

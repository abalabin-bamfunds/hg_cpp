import math
import random
import subprocess
import sys

import pytest

import bg


def test_add_operator_accepts_double_scalars_and_time_series():
    @bg.graph
    def scalar_add() -> bg.TS[float]:
        return bg.operators.add(1.25, 2.5)

    @bg.graph
    def mixed_add(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(value, 2.5)

    @bg.graph
    def reversed_mixed_add(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(2.5, value)

    @bg.graph
    def series_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    @bg.graph
    def keyword_add(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add(rhs=rhs, lhs=lhs)

    @bg.graph
    def promoted_add(lhs: float, rhs: float) -> float:
        return bg.operators.add(lhs, rhs)

    assert bg.test_graph(scalar_add) == [3.75]
    assert bg.test_graph(mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert bg.test_graph(reversed_mixed_add, [1.0, 2.0]) == [3.5, 4.5]
    assert bg.test_graph(series_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]
    assert bg.test_graph(keyword_add, [1.0], [10.0]) == [11.0]
    assert bg.test_graph(promoted_add, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_operator_accepts_i64_scalars_and_time_series():
    @bg.graph
    def scalar_add() -> bg.TS[int]:
        return bg.operators.add(2, 3)

    @bg.graph
    def series_plus_scalar(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.add(value, 3)

    @bg.graph
    def scalar_plus_series(value: bg.TS[int]) -> bg.TS[int]:
        return bg.operators.add(3, value)

    assert bg.test_graph(scalar_add) == [5]
    assert bg.test_graph(series_plus_scalar, [1, -2]) == [4, 1]
    assert bg.test_graph(scalar_plus_series, [1, -2]) == [4, 1]


def test_i64_arithmetic_wraps_at_signed_boundaries():
    minimum = -(1 << 63)
    maximum = (1 << 63) - 1

    @bg.graph
    def add(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs + rhs

    @bg.graph
    def subtract(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs - rhs

    @bg.graph
    def multiply(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs * rhs

    @bg.graph
    def negate(value: bg.TS[int]) -> bg.TS[int]:
        return -value

    @bg.graph
    def absolute(value: bg.TS[int]) -> bg.TS[int]:
        return abs(value)

    assert bg.test_graph(add, [maximum, minimum], [1, -1]) == [minimum, maximum]
    assert bg.test_graph(subtract, [minimum, maximum], [1, -1]) == [maximum, minimum]
    assert bg.test_graph(multiply, [maximum, minimum], [2, -1]) == [-2, minimum]
    assert bg.test_graph(negate, [minimum, maximum]) == [minimum, -maximum]
    assert bg.test_graph(absolute, [minimum, -maximum]) == [minimum, maximum]


def test_python_operator_syntax_and_named_boolean_operators():
    @bg.graph
    def arithmetic(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return abs(-(lhs - rhs) * rhs)

    @bg.graph
    def comparison(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.and_(lhs < rhs, bg.operators.not_(rhs < 0))

    assert bg.test_graph(arithmetic, [5.0, 2.0], [2.0, 3.0]) == [6.0, 3.0]
    assert bg.test_graph(comparison, [1, 4, 3], [2, 3, -1]) == [True, False, False]


def test_collection_query_operators_accept_public_keyword_names():
    class Pair(bg.TimeSeriesSchema):
        left: bg.TS[int]
        right: bg.TS[float]

    @bg.graph
    def fixed_list_length(value: bg.TSL[bg.TS[int], 2]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def fixed_bundle_length(value: bg.TSB[Pair]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def set_length(value: bg.TSS[int]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def set_contains(collection: bg.TSS[int], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def dictionary_length(value: bg.TS[dict[int, int]]) -> bg.TS[int]:
        return bg.operators.len_(value=value)

    @bg.graph
    def dictionary_contains(collection: bg.TS[dict[int, int]], key: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=key)

    @bg.graph
    def dictionary_contains_scalar(collection: bg.TS[dict[int, int]]) -> bg.TS[bool]:
        return bg.operators.contains_(collection=collection, key=2)

    assert bg.test_graph(fixed_list_length, [{0: 10, 1: 20}]) == [2]
    assert bg.test_graph(fixed_bundle_length, [{"left": 10, "right": 20.0}]) == [2]
    assert bg.test_graph(set_length, [set(), {1, 2}, bg.SetDelta(added={3}, removed={1})]) == [0, 2, 2]
    assert bg.test_graph(set_contains, [{1}, {2, bg.Removed(1)}, {3, bg.Removed(2)}], [1, 1, 3]) == [True, False, True]
    assert bg.test_graph(dictionary_length, [{1: 10, 2: 20}, {}]) == [2, 0]
    assert bg.test_graph(dictionary_contains, [{1: 10, 2: 20}, {}, {3: 30}], [2, 1, 3]) == [True, False, True]
    assert bg.test_graph(dictionary_contains_scalar, [{1: 10, 2: 20}, {}, {3: 30}]) == [True, False, False]


def test_numeric_operator_syntax_and_explicit_convert_target():
    @bg.graph
    def divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
        return lhs / rhs

    @bg.graph
    def floor_divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs // rhs

    @bg.graph
    def modulo(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return lhs % rhs

    @bg.graph
    def power(value: bg.TS[float]) -> bg.TS[float]:
        return value**2.0

    @bg.graph
    def round_value(value: bg.TS[float]) -> bg.TS[float]:
        return round(value, 2)

    @bg.graph
    def convert(value: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.convert[bg.TS[float]](value)

    assert bg.test_graph(divide, [-7], [3]) == [-7.0 / 3.0]
    assert bg.test_graph(floor_divide, [-7, 7], [3, -3]) == [-3, -3]
    assert bg.test_graph(modulo, [-7, 7], [3, -3]) == [2, -2]
    assert bg.test_graph(power, [-2.0, 3.0]) == [4.0, 9.0]
    assert bg.test_graph(round_value, [2.675, 1.125, 1.375]) == [2.67, 1.12, 1.38]
    assert bg.test_graph(convert, [2, -3]) == [2.0, -3.0]


def test_integer_division_converts_operands_to_binary64_for_full_i64_domain():
    @bg.graph
    def divide(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
        return lhs / rhs

    minimum = -(1 << 63)
    maximum = (1 << 63) - 1
    two_to_53 = 1 << 53
    two_to_54 = 1 << 54
    lhs = [
        two_to_53 + 1,
        two_to_53 + 3,
        9007199254740993,
        two_to_54 - 1,
        maximum,
        minimum,
        1,
        0,
        27021597764222979,
    ]
    rhs = [two_to_53, two_to_53, 3, 1, 1, -1, maximum, minimum, 864691128455135232]

    random_values = random.Random(0xD1_2026)
    for _ in range(2048):
        lhs.append(random_values.randint(minimum, maximum))
        denominator = random_values.randint(minimum, maximum)
        rhs.append(denominator or 1)

    for _ in range(2048):
        lhs_bits = random_values.randrange(1, 64)
        rhs_bits = random_values.randrange(1, 64)
        numerator = (1 << (lhs_bits - 1)) | random_values.getrandbits(lhs_bits - 1)
        denominator = (1 << (rhs_bits - 1)) | random_values.getrandbits(rhs_bits - 1)
        lhs.append(-numerator if random_values.getrandbits(1) else numerator)
        rhs.append(-denominator if random_values.getrandbits(1) else denominator)

    actual = bg.test_graph(divide, lhs, rhs)
    expected = [float(numerator) / float(denominator) for numerator, denominator in zip(lhs, rhs)]
    assert [value.hex() for value in actual] == [value.hex() for value in expected]

    negative_zero = bg.test_graph(divide, [0], [minimum])[0]
    assert negative_zero == 0.0
    assert math.copysign(1.0, negative_zero) == -1.0


def test_reflected_numeric_operator_syntax():
    @bg.graph
    def divide(value: bg.TS[int]) -> bg.TS[float]:
        return 7 / value

    @bg.graph
    def floor_divide(value: bg.TS[int]) -> bg.TS[int]:
        return 7 // value

    @bg.graph
    def modulo(value: bg.TS[int]) -> bg.TS[int]:
        return 7 % value

    @bg.graph
    def power(value: bg.TS[int]) -> bg.TS[int]:
        return 2**value

    assert bg.test_graph(divide, [2, -2]) == [3.5, -3.5]
    assert bg.test_graph(floor_divide, [2, -2]) == [3, -4]
    assert bg.test_graph(modulo, [3, -3]) == [1, -2]
    assert bg.test_graph(power, [2, 3]) == [4, 8]


def test_mapped_python_operator_wrappers_are_reusable_and_isolated():
    to_float = bg.operators.convert[bg.TS[float]]

    @bg.graph
    def convert_first(value: bg.TS[int]) -> bg.TS[float]:
        return to_float(value)

    @bg.graph
    def convert_second(value: bg.TS[int]) -> bg.TS[float]:
        return to_float(value)

    assert bg.test_graph(convert_first, [1, 2]) == [1.0, 2.0]
    assert bg.test_graph(convert_second, [3, 4]) == [3.0, 4.0]
    with pytest.raises(ValueError, match="does not match operator parameter"):
        to_float[bg.TS[int]]

    @bg.graph
    def explicit_constant() -> bg.TS[bool]:
        return bg.operators.eq[int](1, 1)

    assert bg.test_graph(explicit_constant) == [True]


def test_round_matches_python_for_deterministic_binary64_samples():
    @bg.graph
    def round_value(value: bg.TS[float], digits: bg.TS[int]) -> bg.TS[float]:
        return round(value, digits)

    random_values = random.Random(0xB6_2026)
    values = [random_values.uniform(-1e120, 1e120) for _ in range(1000)]
    digits = [random_values.randrange(-100, 101) for _ in values]
    assert bg.test_graph(round_value, values, digits) == [round(value, ndigits) for value, ndigits in zip(values, digits)]


def test_convert_requires_a_compatible_explicit_full_time_series_type():
    @bg.graph
    def missing_target(value: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.convert(value)

    @bg.graph
    def wrong_target(value: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.convert[bg.TS[str]](value)

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        bg.test_graph(missing_target, [1])

    with pytest.raises(ValueError, match="Cannot resolve operator convert"):
        bg.test_graph(wrong_target, [1])


def test_symbol_equality_handles_scalar_mixed_and_time_series_inputs():
    @bg.graph
    def scalar_equal() -> bg.TS[bool]:
        return bg.operators.eq(bg.Symbol(), bg.symbol(""))

    @bg.graph
    def scalar_not_equal() -> bg.TS[bool]:
        return bg.operators.ne(bg.Symbol(), bg.symbol(""))

    @bg.graph
    def mixed_equal(value: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.eq(value, bg.symbol(""))

    @bg.graph
    def mixed_not_equal(value: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.ne(value, bg.symbol(""))

    @bg.graph
    def series_equal(lhs: bg.TS[bg.Symbol], rhs: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.eq(lhs, rhs)

    @bg.graph
    def series_not_equal(lhs: bg.TS[bg.Symbol], rhs: bg.TS[bg.Symbol]) -> bg.TS[bool]:
        return bg.operators.ne(lhs, rhs)

    values = [bg.Symbol(), bg.symbol(""), bg.symbol("bid")]
    same_or_different = [bg.symbol(""), bg.symbol(""), bg.symbol("ask")]

    assert bg.test_graph(scalar_equal) == [False]
    assert bg.test_graph(scalar_not_equal) == [True]
    assert bg.test_graph(mixed_equal, values) == [False, True, False]
    assert bg.test_graph(mixed_not_equal, values) == [True, False, True]
    assert bg.test_graph(series_equal, values, same_or_different) == [False, True, False]
    assert bg.test_graph(series_not_equal, values, same_or_different) == [True, False, True]


@pytest.mark.parametrize(
    "expression",
    [
        bool,
        lambda point: point and point,
        lambda point: point or point,
        lambda point: not point,
        lambda point: point < 3 < point,
    ],
    ids=["bool", "and", "or", "not", "chained-comparison"],
)
def test_python_wiring_points_reject_truth_conversion(expression):
    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[int]:
        return expression(value)

    with pytest.raises(TypeError, match="wiring point has no Python truth value"):
        bg.test_graph(graph, [1])


def test_add_operator_concatenates_python_strings():
    @bg.graph
    def scalar_add() -> bg.TS[str]:
        return bg.operators.add("left", "right")

    @bg.graph
    def series_add(value: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.add(value, "\x00suffix")

    assert bg.test_graph(scalar_add) == ["leftright"]
    assert bg.test_graph(series_add, ["a\x00b", "café"]) == ["a\x00b\x00suffix", "café\x00suffix"]


def test_add_accepts_a_native_const_wiring_point_and_propagates_its_guarantee():
    @bg.graph
    def graph() -> bg.TS[float]:
        constant = bg.operators.const_(4.0)
        result = bg.operators.add(constant, 1.0)
        assert result.type_name == "TS<double, const>"
        return result

    assert bg.test_graph(graph) == [5.0]


def test_wiring_point_addition_uses_the_native_operator():
    @bg.node
    def passthrough(value: float) -> float:
        return value

    @bg.graph
    def add_nested(lhs: bg.TS[float], rhs: float) -> bg.TS[float]:
        return lhs + rhs

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        left = passthrough(lhs)
        right = passthrough(rhs)
        forward = left + right
        return add_nested(forward, 2.0)

    @bg.graph
    def reflected(value: bg.TS[float]) -> bg.TS[float]:
        return 2.0 + value

    assert bg.test_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [13.0, 24.0]
    assert bg.test_graph(reflected, [3.0, 4.0]) == [5.0, 6.0]


def test_add_operator_accepts_values_dereferenced_from_references():
    @bg.compute_node
    def forward(value: bg.REF[bg.TS[float]]) -> bg.REF[bg.TS[float]]:
        return value.value

    @bg.graph
    def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
        return forward(lhs) + forward(rhs)

    assert bg.test_graph(graph, [1.0, 2.0], [10.0, 20.0]) == [11.0, 22.0]


def test_add_observes_retargeted_references_with_equal_values():
    @bg.source_node
    def first() -> bg.TS[float]:
        return 4.0

    @bg.source_node
    def second() -> bg.TS[float]:
        return 4.0

    @bg.compute_node
    def select(
        use_second: bg.TS[bool],
        lhs: bg.REF[bg.TS[float]],
        rhs: bg.REF[bg.TS[float]],
    ) -> bg.REF[bg.TS[float]]:
        return (rhs if use_second.value else lhs).value

    @bg.graph
    def graph(use_second: bg.TS[bool]) -> bg.TS[float]:
        selected = select(use_second, first(), second())
        return bg.operators.add(selected, 1.0)

    assert bg.test_graph(graph, [False, True, False, True]) == [5.0, 5.0, 5.0, 5.0]


def test_reflected_add_preserves_operand_order_and_native_diagnostics():
    @bg.graph
    def unsupported_direct(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.add("unsupported", value)

    @bg.graph
    def unsupported_reflected(value: bg.TS[float]) -> bg.TS[float]:
        return "unsupported" + value

    with pytest.raises(ValueError, match="Cannot resolve operator add") as raised:
        bg.test_graph(unsupported_direct, [1.0])

    with pytest.raises(ValueError, match="Cannot resolve operator add") as reflected:
        bg.test_graph(unsupported_reflected, [1.0])

    assert "Arguments: (lhs: string, rhs:" in str(raised.value)
    assert str(reflected.value) == str(raised.value)


def test_add_operator_preserves_required_input_validity():
    import bg._bg_test_extension as extension

    @bg.compute_node
    def to_double(value: bg.TS[int]) -> bg.TS[float]:
        return float(value.value)

    @bg.graph
    def graph(value: bg.TS[int], rhs: bg.TS[float]) -> bg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(value))
        return lhs + rhs

    assert bg.test_graph(graph, [1, 0, 3], [10.0, 20.0, 30.0]) == [11.0, None, 33.0]


def test_add_does_not_schedule_on_start_for_an_initially_invalid_input():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return value + 1.0

    assert bg.test_graph(graph, [None, 2.0]) == [None, 3.0]


def test_default_operator_is_available_with_native_fallback_behavior():
    @bg.graph
    def graph(value: bg.TS[float]) -> bg.TS[float]:
        return bg.operators.default_(value, fallback=5.0)

    assert bg.test_graph(graph, [None, 1.0, 3.0]) == [5.0, 1.0, 3.0]


def test_add_optional_overload_substitutes_zero_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.node
    def to_double(value: int) -> float:
        return float(value)

    @bg.graph
    def optional_series_plus_scalar(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return bg.operators.add(bg.opt(value), 1.0)

    @bg.graph
    def scalar_plus_optional_series(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return 1.0 + bg.opt(value)

    @bg.graph
    def optional_series_plus_series(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[float]:
        lhs = to_double(extension.invalidate_on_zero(lhs))
        rhs = to_double(extension.invalidate_on_zero(rhs))
        return bg.opt(lhs) + rhs

    assert bg.test_graph(optional_series_plus_scalar, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert bg.test_graph(scalar_plus_optional_series, [2, 0, 3]) == [3.0, 1.0, 4.0]
    assert bg.test_graph(optional_series_plus_series, [2, 0, 3], [10, 20, 0]) == [12.0, 20.0, 3.0]


def test_add_optional_i64_overload_substitutes_zero_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[int]:
        return bg.opt(extension.invalidate_on_zero(lhs)) + bg.opt(extension.invalidate_on_zero(rhs))

    assert bg.test_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == [11, 20, 3, 0]


def test_add_optional_string_overload_substitutes_empty_string_for_invalid_inputs():
    import bg._bg_test_extension as extension

    @bg.node
    def to_string(value: int) -> str:
        return str(value)

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[str]:
        left = to_string(extension.invalidate_on_zero(lhs))
        right = to_string(extension.invalidate_on_zero(rhs))
        return bg.opt(left) + right

    assert bg.test_graph(graph, [1, 0, 3, 0], [10, 20, 0, 0]) == ["110", "20", "3", ""]


def test_optional_string_addition_waits_for_ticks_and_retains_latest_inputs():
    @bg.node
    def to_string(value: int) -> str:
        return str(value)

    @bg.graph
    def graph(lhs: bg.TS[int], rhs: bg.TS[int]) -> bg.TS[str]:
        return bg.opt(to_string(lhs)) + bg.opt(to_string(rhs))

    assert bg.test_graph(graph, [None, 1, None, 2], [None, None, 3, None]) == [None, "1", "13", "23"]


def test_wiring_point_addition_selects_optional_overload():
    import bg._bg_test_extension as extension

    @bg.node
    def to_double(value: int) -> float:
        return float(value)

    @bg.graph
    def graph(value: bg.TS[int]) -> bg.TS[float]:
        value = to_double(extension.invalidate_on_zero(value))
        return bg.opt(value) + 1.0

    assert bg.test_graph(graph, [2, 0, 3]) == [3.0, 1.0, 4.0]


def test_wiring_point_addition_does_not_depend_on_python_operator_attributes():
    script = """
import gc
import bg
import bg._bg as native

del bg.operators.add
del native.operators.add
gc.collect()
bg.operators.add = object()
native.operators.add = object()
gc.collect()

@bg.graph
def graph(lhs: bg.TS[float], rhs: bg.TS[float]) -> bg.TS[float]:
    return lhs + rhs

@bg.graph
def reflected(value: bg.TS[float]) -> bg.TS[float]:
    return 2.0 + value

assert bg.test_graph(graph, [1.0], [2.0]) == [3.0]
assert bg.test_graph(reflected, [3.0]) == [5.0]
"""

    completed = subprocess.run([sys.executable, "-c", script], check=False, capture_output=True, text=True)

    assert completed.returncode == 0, completed.stderr
    assert "nanobind: leaked" not in completed.stderr


def test_add_reports_native_overload_diagnostics_for_unsupported_types():
    @bg.graph
    def graph(lhs: bg.TS[bool], rhs: bg.TS[bool]) -> bg.TS[bool]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.test_graph(graph, [True], [False])

    @bg.graph
    def mixed(lhs: bg.TS[float], rhs: bg.TS[int]) -> bg.TS[float]:
        return bg.operators.add(lhs, rhs)

    with pytest.raises(ValueError, match="Cannot resolve operator add"):
        bg.test_graph(mixed, [1.0], [2])


def test_operator_namespace_reexports_existing_higher_order_apis():
    import bg._bg as native

    assert bg.operators.add is native.operators.add
    assert bg.operators.map is bg.map
    assert bg.operators.mesh is bg.mesh
    assert bg.operators.switch_ is bg.switch_


def test_addition_checks_wiring_point_lifetime():
    saved_points = []

    @bg.graph
    def save(value: bg.TS[float]) -> bg.TS[float]:
        saved_points.append(value)
        return value

    assert bg.test_graph(save, [1.0]) == [1.0]

    @bg.graph
    def use_expired(value: bg.TS[float]) -> bg.TS[float]:
        return 1.0 + saved_points[0]

    with pytest.raises(RuntimeError, match="outside its graph construction"):
        bg.test_graph(use_expired, [2.0])

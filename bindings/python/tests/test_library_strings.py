import pytest

import bg


def test_string_multiplication_accepts_scalar_and_time_series_operands():
    @bg.graph
    def scalar() -> bg.TS[str]:
        return bg.operators.mul("ab", 3)

    @bg.graph
    def text_by_scalar(value: bg.TS[str]) -> bg.TS[str]:
        return value * 2

    @bg.graph
    def scalar_by_text(value: bg.TS[str]) -> bg.TS[str]:
        return 2 * value

    @bg.graph
    def scalar_text_by_series(count: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.mul("xy", count)

    assert bg.test_graph(scalar) == ["ababab"]
    assert bg.test_graph(text_by_scalar, ["a", "bc"]) == ["aa", "bcbc"]
    assert bg.test_graph(scalar_by_text, ["a", "bc"]) == ["aa", "bcbc"]
    assert bg.test_graph(scalar_text_by_series, [2, 3]) == ["xyxy", "xyxyxy"]


def test_string_multiplication_preserves_bytes_and_handles_empty_or_nonpositive_counts():
    @bg.graph
    def repeat(value: bg.TS[str], count: bg.TS[int]) -> bg.TS[str]:
        return value * count

    assert bg.test_graph(repeat, ["ab", "ab", "ab", "", "a\0b"], [0, -2, 2, 2, 2]) == [
        "",
        "",
        "abab",
        "",
        "a\0ba\0b",
    ]


def test_substr_accepts_two_or_three_arguments_and_clamps_code_point_indexes():
    @bg.graph
    def to_end(value: bg.TS[str], start: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.substr(value, start)

    @bg.graph
    def with_end(value: bg.TS[str], start: bg.TS[int], end: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.substr(value, start, end)

    assert bg.test_graph(to_end, ["abcde"] * 5, [-2, -99, 5, -(1 << 63), (1 << 63) - 1]) == [
        "de",
        "abcde",
        "",
        "abcde",
        "",
    ]
    assert bg.test_graph(
        with_end,
        ["abcde", "abcde", "abcde", "abcde", "a\0bc"],
        [-4, 1, 3, -(1 << 63), 1],
        [-1, -(1 << 63), 1, (1 << 63) - 1, 3],
    ) == ["bcd", "", "", "abcde", "\0b"]


def test_string_getitem_indexes_code_points_and_rejects_out_of_range_indexes():
    @bg.graph
    def index(value: bg.TS[str], position: bg.TS[int]) -> bg.TS[str]:
        return value[position]

    assert bg.test_graph(index, ["abc", "abc", "abc", "a\0b"], [0, -1, -3, 1]) == ["a", "c", "a", "\0"]

    with pytest.raises(IndexError):
        bg.test_graph(index, ["abc"], [3])


def test_unicode_string_length_indexing_and_slicing_count_code_points():
    @bg.graph
    def length(value: bg.TS[str]) -> bg.TS[int]:
        return bg.operators.len_(value)

    @bg.graph
    def index(value: bg.TS[str], position: bg.TS[int]) -> bg.TS[str]:
        return value[position]

    @bg.graph
    def slice(value: bg.TS[str], start: bg.TS[int], end: bg.TS[int]) -> bg.TS[str]:
        return bg.operators.substr(value, start, end)

    text = "é🍕é"
    assert bg.test_graph(length, [text, "a\0b"]) == [4, 3]
    assert bg.test_graph(index, [text] * 4, [0, 1, -1, -2]) == ["é", "🍕", "́", "e"]
    assert bg.test_graph(slice, [text] * 3, [0, 2, 1], [2, 4, 3]) == ["é🍕", "é", "🍕e"]


def test_unicode_string_case_conversion_uses_root_locale_mappings():
    @bg.graph
    def lower(value: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.lower(value)

    @bg.graph
    def upper(value: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.upper(value)

    @bg.graph
    def casefold(value: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.casefold(value)

    assert bg.test_graph(lower, ["ΟΣ", "𐐀", "é"]) == ["ος", "𐐨", "é"]
    assert bg.test_graph(upper, ["ß", "é"]) == ["SS", "É"]
    assert bg.test_graph(casefold, ["Straße"]) == ["strasse"]


def test_string_operations_invalidate_and_revalidate_with_required_inputs():
    @bg.graph
    def result_valid(value: bg.TS[str], count: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.valid(value * count)

    assert bg.test_graph(result_valid, [None, bg.INVALID, "ab", None, bg.INVALID, "cd"], [2] * 6) == [
        False,
        None,
        True,
        None,
        False,
        True,
    ]

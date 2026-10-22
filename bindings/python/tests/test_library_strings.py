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


def test_matches_searches_unicode_text_and_discards_capture_results():
    @bg.graph
    def matches(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches(r"(?<word>café)", text)

    assert bg.test_graph(matches, ["un café ici", "café", "caf", "🍕 café!"]) == [True, True, False, True]


def test_matches_accepts_symbol_scalars():
    @bg.graph
    def matches() -> bg.TS[bool]:
        return bg.operators.matches(bg.symbol("(?<word>café)"), bg.symbol("un café ici"))

    assert bg.test_graph(matches) == [True]


def test_matches_and_replace_accept_mixed_string_and_symbol_inputs():
    @bg.graph
    def matches(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches(bg.symbol("café"), text)

    @bg.graph
    def replace(text: bg.TS[bg.Symbol]) -> bg.TS[str]:
        return bg.operators.replace("x", bg.symbol("💠"), text)

    assert bg.test_graph(matches, ["un café", "none"]) == [True, False]
    assert bg.test_graph(replace, [bg.symbol("xx"), bg.symbol("none")]) == ["💠💠", "none"]


def test_match_accepts_a_symbol_pattern_and_symbol_subject():
    @bg.graph
    def captures(text: bg.TS[bg.Symbol]) -> bg.TSL[bg.TS[str], 1]:
        return bg.operators.match_(bg.symbol("(x)"), text)

    assert bg.test_graph(captures, [bg.symbol("x"), bg.symbol("none")]) == [{0: "x"}, None]


def test_regex_operators_follow_reference_retargeting():
    @bg.graph
    def matches(condition: bg.TS[bool], left: bg.TS[str], right: bg.TS[str]) -> bg.TS[bool]:
        selected = bg.operators.if_then_else(condition, left, right)
        return bg.operators.matches("needle", selected)

    @bg.graph
    def replace(condition: bg.TS[bool], left: bg.TS[str], right: bg.TS[str]) -> bg.TS[str]:
        selected = bg.operators.if_then_else(condition, left, right)
        return bg.operators.replace("needle", "found", selected)

    conditions = [True, False, False, True]
    left = ["needle", "plain", "needle", "plain"]
    right = ["plain", "needle", "plain", "needle"]
    assert bg.test_graph(matches, conditions, left, right) == [True, True, False, False]
    assert bg.test_graph(replace, conditions, left, right) == ["found", "found", "plain", "plain"]


def test_replace_supports_python_group_references_and_literal_dollar():
    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace(r"(?<word>é)(?<optional>x)?", r"\g<word>2-$-\\", text)

    assert bg.test_graph(replace, ["éx and é", "none"]) == ["é2-$-\\ and é2-$-\\", "none"]


def test_replace_supports_numbered_named_and_whole_match_references():
    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace(r"(?<word>foo)", r"\1|\g<0>|\g<word>", text)

    @bg.graph
    def backreference(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches(r"(.)\1", text)

    assert bg.test_graph(replace, ["foo and foo", "none"]) == ["foo|foo|foo and foo|foo|foo", "none"]
    assert bg.test_graph(backreference, ["book", "abc"]) == [True, False]


def test_replace_emits_unmatched_groups_as_empty_and_decodes_python_octal_escapes():
    @bg.graph
    def unmatched(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace(r"(a)?b", r"\1x", text)

    @bg.graph
    def octal(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\377", text)

    assert bg.test_graph(unmatched, ["b", "ab"]) == ["x", "ax"]
    assert bg.test_graph(octal, ["x", "no x"]) == ["ÿ", "no ÿ"]


def test_replace_follows_zero_length_match_progression():
    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x*", "_", text)

    assert bg.test_graph(replace, ["abxd", ""]) == ["_a_b__d_", "_"]


def test_replace_preserves_embedded_nul_astral_text_and_python_escapes():
    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("(🍕)", "a\0b", text)

    @bg.graph
    def escapes(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\n\t\r\a\b\f\v", text)

    @bg.graph
    def punctuation(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\&\.", text)

    assert bg.test_graph(replace, ["left🍕right", "🍕\0🍕"]) == [
        "lefta\0bright",
        "a\0b\0a\0b",
    ]
    assert bg.test_graph(escapes, ["x"]) == ["\n\t\r\a\b\f\v"]
    assert bg.test_graph(punctuation, ["x"]) == [r"\&\."]


def test_regex_patterns_and_captures_preserve_embedded_nul():
    @bg.graph
    def matches(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches("a\0b", text)

    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("(a\0b)", "X", text)

    @bg.graph
    def captures(text: bg.TS[str]) -> bg.TSL[bg.TS[str], 1]:
        return bg.operators.match_("(a\0b)", text)

    assert bg.test_graph(matches, ["xa\0by"]) == [True]
    assert bg.test_graph(replace, ["xa\0by"]) == ["xXy"]
    assert bg.test_graph(captures, ["xa\0by"]) == [{0: "a\0b"}]


def test_match_returns_ordered_captures_and_invalidates_unmatched_groups():
    @bg.graph
    def captures(text: bg.TS[str]) -> bg.TSL[bg.TS[str], 2]:
        return bg.operators.match_(r"(?<word>é)(?<optional>x)?", text)

    assert bg.test_graph(captures, ["éx", "é", "none"]) == [
        {0: "é", 1: "x"},
        {0: "é", 1: bg.INVALID},
        None,
    ]


def test_match_requires_captures_and_all_unmatched_groups_leave_the_list_invalid():
    @bg.graph
    def no_captures(text: bg.TS[str]) -> bg.TSL[bg.TS[str], 0]:
        return bg.operators.match_("x", text)

    @bg.graph
    def captures_valid(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.match_(r"(?:(a)?(b)?)", text))

    @bg.graph
    def matched(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches(r"(?:(a)?(b)?)", text)

    with pytest.raises(ValueError, match=r"use matches\(\)"):
        bg.test_graph(no_captures, ["x"])
    assert bg.test_graph(captures_valid, [""]) == [False]
    assert bg.test_graph(matched, [""]) == [True]


def test_match_preserves_empty_captures_omits_absent_groups_and_revalidates_after_match_loss():
    @bg.graph
    def empty_and_absent(text: bg.TS[str]) -> bg.TSL[bg.TS[str], 2]:
        return bg.operators.match_(r"(x)?()", text)

    @bg.graph
    def captures(text: bg.TS[str]) -> bg.TSL[bg.TS[str], 1]:
        return bg.operators.match_("(x)", text)

    assert bg.test_graph(empty_and_absent, ["", "x"]) == [{1: ""}, {0: "x", 1: ""}]
    assert bg.test_graph(captures, ["x", "none", "x"]) == [{0: "x"}, None, {0: "x"}]


def test_regex_operators_invalidate_and_revalidate_with_required_subjects():
    @bg.graph
    def matches(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches("x", text)

    @bg.graph
    def replace(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", "y", text)

    values = ["x", bg.INVALID, "z"]
    assert bg.test_graph(matches, values) == [True, None, False]
    assert bg.test_graph(replace, values) == ["y", None, "z"]


def test_regex_rejects_surrogates_before_utf8_conversion():
    @bg.graph
    def malformed_pattern(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches("\ud800", text)

    @bg.graph
    def malformed_replacement(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", "\ud800", text)

    @bg.graph
    def malformed_subject(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches("x", text)

    with pytest.raises(UnicodeEncodeError):
        bg.test_graph(malformed_pattern, ["x"])
    with pytest.raises(UnicodeEncodeError):
        bg.test_graph(malformed_replacement, ["x"])
    with pytest.raises(UnicodeEncodeError):
        bg.test_graph(malformed_subject, ["\ud800"])


def test_regex_pattern_and_replacement_errors_precede_search():
    @bg.graph
    def invalid_pattern(text: bg.TS[str]) -> bg.TS[bool]:
        return bg.operators.matches("(", text)

    @bg.graph
    def invalid_group(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\g<2>", text)

    @bg.graph
    def invalid_octal(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\400", text)

    @bg.graph
    def invalid_escape(text: bg.TS[str]) -> bg.TS[str]:
        return bg.operators.replace("x", r"\q", text)

    with pytest.raises(ValueError, match="regular expression"):
        bg.test_graph(invalid_pattern, ["no match"])
    with pytest.raises(ValueError, match="invalid group reference"):
        bg.test_graph(invalid_group, ["no match"])
    with pytest.raises(ValueError, match="octal escape"):
        bg.test_graph(invalid_octal, ["no match"])
    with pytest.raises(ValueError, match="bad escape"):
        bg.test_graph(invalid_escape, ["no match"])


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


def test_split_defaults_to_unlimited_and_preserves_embedded_bytes():
    @bg.graph
    def split(value: bg.TS[str]) -> bg.TS[tuple[str, ...]]:
        return bg.operators.split(value, "💠")

    assert bg.test_graph(split, ["a💠b💠c", "a\0💠é", "💠a💠"]) == [
        ("a", "b", "c"),
        ("a\0", "é"),
        ("", "a", ""),
    ]


def test_split_maxsplit_preserves_the_final_remainder():
    @bg.graph
    def split(value: bg.TS[str], maxsplit: bg.TS[int]) -> bg.TS[tuple[str, ...]]:
        return bg.operators.split(value, ",", maxsplit)

    assert bg.test_graph(split, ["a,b,c", "a,b,c", "a,b,c"], [0, 1, -1]) == [
        ("a,b,c",),
        ("a", "b,c"),
        ("a", "b", "c"),
    ]


def test_split_accepts_an_explicit_fixed_tsl_result():
    @bg.graph
    def split(value: bg.TS[str]) -> bg.TSL[bg.TS[str], 2]:
        return bg.operators.split[bg.TSL[bg.TS[str], 2]](value, ",")

    assert bg.test_graph(split, ["a,b", "x,y,z"]) == [
        {0: "a", 1: "b"},
        {0: "x", 1: "y,z"},
    ]

    with pytest.raises(ValueError, match="fewer pieces"):
        bg.test_graph(split, ["a"])


def test_join_accepts_scalar_lists_and_fixed_time_series_lists():
    @bg.graph
    def join_list(values: bg.TS[tuple[str, ...]]) -> bg.TS[str]:
        return bg.operators.join(values, "|")

    @bg.graph
    def join_tsl(values: bg.TSL[bg.TS[str], 2]) -> bg.TS[str]:
        return bg.operators.join(values, "💠")

    assert bg.test_graph(join_list, [("a", "b", "c"), (), ("é", "x\0y")]) == [
        "a|b|c",
        "",
        "é|x\0y",
    ]
    assert bg.test_graph(join_tsl, [{0: "a", 1: "b"}, {0: "x", 1: "y"}]) == [
        "a💠b",
        "x💠y",
    ]


def test_optional_join_skips_invalid_items_and_treats_an_invalid_root_as_empty():
    @bg.graph
    def join(values: bg.TSL[bg.TS[str], 2]) -> bg.TS[str]:
        return bg.operators.join(bg.opt(values), "|")

    assert bg.test_graph(join, [{0: "a", 1: bg.INVALID}, bg.INVALID, {0: "x", 1: "y"}]) == [
        "a",
        "",
        "x|y",
    ]


def test_str_renders_nested_scalar_lists_and_builtin_values():
    @bg.graph
    def render(values: bg.TS[tuple[int, ...]]) -> bg.TS[str]:
        return bg.operators.str_(values)

    @bg.graph
    def render_boolean(value: bg.TS[bool]) -> bg.TS[str]:
        return bg.operators.str_(value)

    assert bg.test_graph(render, [(1, 2), ()]) == ["[1, 2]", "[]"]
    assert bg.test_graph(render_boolean, [True, False]) == ["True", "False"]


def test_format_uses_positional_and_named_native_format_specs():
    @bg.graph
    def positional(value: bg.TS[int], ratio: bg.TS[float]) -> bg.TS[str]:
        return bg.operators.format_("{:04d} {:.2f}", value, ratio)

    @bg.graph
    def named(value: bg.TS[float]) -> bg.TS[str]:
        return bg.operators.format_("{value:.2f}", value=value)

    @bg.graph
    def no_arguments() -> bg.TS[str]:
        return bg.operators.format_("static\0text")

    assert bg.test_graph(positional, [7, -12], [1.25, 2.5]) == ["0007 1.25", "-012 2.50"]
    assert bg.test_graph(named, [1.25, 2.5]) == ["1.25", "2.50"]
    assert bg.test_graph(no_arguments) == ["static\0text"]


def test_print_writes_scalar_and_time_series_patterns_to_selected_stream(capfd):
    @bg.graph
    def emit(value: bg.TS[int], pattern: bg.TS[str]) -> bg.TS[int]:
        bg.operators.print_("value={:d}", value)
        bg.operators.print_(pattern, value)
        bg.operators.print_("error\0{:d}", value, __std_out__=False)
        return bg.operators.const_(0)

    assert bg.test_graph(emit, [7, 8], ["first={:d}", "second={:d}"]) == [0, None]
    captured = capfd.readouterr()
    assert captured.out == "value=7\nfirst=7\nvalue=8\nsecond=8\n"
    assert captured.err == "error\0" + "7\n" + "error\0" + "8\n"

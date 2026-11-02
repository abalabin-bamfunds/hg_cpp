import pytest

import hgraph as hg


def test_string_multiplication_accepts_scalar_and_time_series_operands():
    @hg.graph
    def scalar() -> hg.TS[str]:
        return hg.operators.mul("ab", 3)

    @hg.graph
    def text_by_scalar(value: hg.TS[str]) -> hg.TS[str]:
        return value * 2

    @hg.graph
    def scalar_by_text(value: hg.TS[str]) -> hg.TS[str]:
        return 2 * value

    @hg.graph
    def scalar_text_by_series(count: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.mul("xy", count)

    assert hg.run_graph(scalar) == ["ababab"]
    assert hg.run_graph(text_by_scalar, ["a", "bc"]) == ["aa", "bcbc"]
    assert hg.run_graph(scalar_by_text, ["a", "bc"]) == ["aa", "bcbc"]
    assert hg.run_graph(scalar_text_by_series, [2, 3]) == ["xyxy", "xyxyxy"]


def test_string_multiplication_preserves_bytes_and_handles_empty_or_nonpositive_counts():
    @hg.graph
    def repeat(value: hg.TS[str], count: hg.TS[int]) -> hg.TS[str]:
        return value * count

    assert hg.run_graph(repeat, ["ab", "ab", "ab", "", "a\0b"], [0, -2, 2, 2, 2]) == [
        "",
        "",
        "abab",
        "",
        "a\0ba\0b",
    ]


def test_substr_accepts_two_or_three_arguments_and_clamps_code_point_indexes():
    @hg.graph
    def to_end(value: hg.TS[str], start: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.substr(value, start)

    @hg.graph
    def with_end(value: hg.TS[str], start: hg.TS[int], end: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.substr(value, start, end)

    assert hg.run_graph(to_end, ["abcde"] * 5, [-2, -99, 5, -(1 << 63), (1 << 63) - 1]) == [
        "de",
        "abcde",
        "",
        "abcde",
        "",
    ]
    assert hg.run_graph(
        with_end,
        ["abcde", "abcde", "abcde", "abcde", "a\0bc"],
        [-4, 1, 3, -(1 << 63), 1],
        [-1, -(1 << 63), 1, (1 << 63) - 1, 3],
    ) == ["bcd", "", "", "abcde", "\0b"]


def test_string_getitem_indexes_code_points_and_rejects_out_of_range_indexes():
    @hg.graph
    def index(value: hg.TS[str], position: hg.TS[int]) -> hg.TS[str]:
        return value[position]

    assert hg.run_graph(index, ["abc", "abc", "abc", "a\0b"], [0, -1, -3, 1]) == ["a", "c", "a", "\0"]

    with pytest.raises(IndexError):
        hg.run_graph(index, ["abc"], [3])


def test_unicode_string_length_indexing_and_slicing_count_code_points():
    @hg.graph
    def length(value: hg.TS[str]) -> hg.TS[int]:
        return hg.operators.len_(value)

    @hg.graph
    def index(value: hg.TS[str], position: hg.TS[int]) -> hg.TS[str]:
        return value[position]

    @hg.graph
    def slice(value: hg.TS[str], start: hg.TS[int], end: hg.TS[int]) -> hg.TS[str]:
        return hg.operators.substr(value, start, end)

    text = "é🍕é"
    assert hg.run_graph(length, [text, "a\0b"]) == [4, 3]
    assert hg.run_graph(index, [text] * 4, [0, 1, -1, -2]) == ["é", "🍕", "́", "e"]
    assert hg.run_graph(slice, [text] * 3, [0, 2, 1], [2, 4, 3]) == ["é🍕", "é", "🍕e"]


def test_unicode_string_case_conversion_uses_root_locale_mappings():
    @hg.graph
    def lower(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.lower(value)

    @hg.graph
    def upper(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.upper(value)

    @hg.graph
    def casefold(value: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.casefold(value)

    assert hg.run_graph(lower, ["ΟΣ", "𐐀", "é"]) == ["ος", "𐐨", "é"]
    assert hg.run_graph(upper, ["ß", "é"]) == ["SS", "É"]
    assert hg.run_graph(casefold, ["Straße"]) == ["strasse"]


def test_matches_searches_unicode_text_and_discards_capture_results():
    @hg.graph
    def matches(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches(r"(?<word>café)", text)

    assert hg.run_graph(matches, ["un café ici", "café", "caf", "🍕 café!"]) == [True, True, False, True]


def test_matches_accepts_symbol_scalars():
    @hg.graph
    def matches() -> hg.TS[bool]:
        return hg.operators.matches(hg.symbol("(?<word>café)"), hg.symbol("un café ici"))

    assert hg.run_graph(matches) == [True]


def test_matches_and_replace_accept_mixed_string_and_symbol_inputs():
    @hg.graph
    def matches(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches(hg.symbol("café"), text)

    @hg.graph
    def replace(text: hg.TS[hg.Symbol]) -> hg.TS[str]:
        return hg.operators.replace("x", hg.symbol("💠"), text)

    assert hg.run_graph(matches, ["un café", "none"]) == [True, False]
    assert hg.run_graph(replace, [hg.symbol("xx"), hg.symbol("none")]) == ["💠💠", "none"]


def test_match_accepts_a_symbol_pattern_and_symbol_subject():
    @hg.graph
    def captures(text: hg.TS[hg.Symbol]) -> hg.TSL[hg.TS[str], 1]:
        return hg.operators.match_(hg.symbol("(x)"), text)

    assert hg.run_graph(captures, [hg.symbol("x"), hg.symbol("none")]) == [{0: "x"}, None]


def test_regex_operators_follow_reference_retargeting():
    @hg.graph
    def matches(condition: hg.TS[bool], left: hg.TS[str], right: hg.TS[str]) -> hg.TS[bool]:
        selected = hg.operators.if_then_else(condition, left, right)
        return hg.operators.matches("needle", selected)

    @hg.graph
    def replace(condition: hg.TS[bool], left: hg.TS[str], right: hg.TS[str]) -> hg.TS[str]:
        selected = hg.operators.if_then_else(condition, left, right)
        return hg.operators.replace("needle", "found", selected)

    conditions = [True, False, False, True]
    left = ["needle", "plain", "needle", "plain"]
    right = ["plain", "needle", "plain", "needle"]
    assert hg.run_graph(matches, conditions, left, right) == [True, True, False, False]
    assert hg.run_graph(replace, conditions, left, right) == ["found", "found", "plain", "plain"]


def test_replace_supports_python_group_references_and_literal_dollar():
    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace(r"(?<word>é)(?<optional>x)?", r"\g<word>2-$-\\", text)

    assert hg.run_graph(replace, ["éx and é", "none"]) == ["é2-$-\\ and é2-$-\\", "none"]


def test_replace_supports_numbered_named_and_whole_match_references():
    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace(r"(?<word>foo)", r"\1|\g<0>|\g<word>", text)

    @hg.graph
    def backreference(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches(r"(.)\1", text)

    assert hg.run_graph(replace, ["foo and foo", "none"]) == ["foo|foo|foo and foo|foo|foo", "none"]
    assert hg.run_graph(backreference, ["book", "abc"]) == [True, False]


def test_replace_emits_unmatched_groups_as_empty_and_decodes_python_octal_escapes():
    @hg.graph
    def unmatched(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace(r"(a)?b", r"\1x", text)

    @hg.graph
    def octal(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\377", text)

    assert hg.run_graph(unmatched, ["b", "ab"]) == ["x", "ax"]
    assert hg.run_graph(octal, ["x", "no x"]) == ["ÿ", "no ÿ"]


def test_replace_follows_zero_length_match_progression():
    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x*", "_", text)

    assert hg.run_graph(replace, ["abxd", ""]) == ["_a_b__d_", "_"]


def test_replace_preserves_embedded_nul_astral_text_and_python_escapes():
    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("(🍕)", "a\0b", text)

    @hg.graph
    def escapes(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\n\t\r\a\b\f\v", text)

    @hg.graph
    def punctuation(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\&\.", text)

    assert hg.run_graph(replace, ["left🍕right", "🍕\0🍕"]) == [
        "lefta\0bright",
        "a\0b\0a\0b",
    ]
    assert hg.run_graph(escapes, ["x"]) == ["\n\t\r\a\b\f\v"]
    assert hg.run_graph(punctuation, ["x"]) == [r"\&\."]


def test_regex_patterns_and_captures_preserve_embedded_nul():
    @hg.graph
    def matches(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches("a\0b", text)

    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("(a\0b)", "X", text)

    @hg.graph
    def captures(text: hg.TS[str]) -> hg.TSL[hg.TS[str], 1]:
        return hg.operators.match_("(a\0b)", text)

    assert hg.run_graph(matches, ["xa\0by"]) == [True]
    assert hg.run_graph(replace, ["xa\0by"]) == ["xXy"]
    assert hg.run_graph(captures, ["xa\0by"]) == [{0: "a\0b"}]


def test_match_returns_ordered_captures_and_invalidates_unmatched_groups():
    @hg.graph
    def captures(text: hg.TS[str]) -> hg.TSL[hg.TS[str], 2]:
        return hg.operators.match_(r"(?<word>é)(?<optional>x)?", text)

    assert hg.run_graph(captures, ["éx", "é", "none"]) == [
        {0: "é", 1: "x"},
        {0: "é", 1: hg.INVALID},
        None,
    ]


def test_match_requires_captures_and_all_unmatched_groups_leave_the_list_invalid():
    @hg.graph
    def no_captures(text: hg.TS[str]) -> hg.TSL[hg.TS[str], 0]:
        return hg.operators.match_("x", text)

    @hg.graph
    def captures_valid(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.match_(r"(?:(a)?(b)?)", text))

    @hg.graph
    def matched(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches(r"(?:(a)?(b)?)", text)

    with pytest.raises(ValueError, match=r"use matches\(\)"):
        hg.run_graph(no_captures, ["x"])
    assert hg.run_graph(captures_valid, [""]) == [False]
    assert hg.run_graph(matched, [""]) == [True]


def test_match_preserves_empty_captures_omits_absent_groups_and_revalidates_after_match_loss():
    @hg.graph
    def empty_and_absent(text: hg.TS[str]) -> hg.TSL[hg.TS[str], 2]:
        return hg.operators.match_(r"(x)?()", text)

    @hg.graph
    def captures(text: hg.TS[str]) -> hg.TSL[hg.TS[str], 1]:
        return hg.operators.match_("(x)", text)

    assert hg.run_graph(empty_and_absent, ["", "x"]) == [{1: ""}, {0: "x", 1: ""}]
    assert hg.run_graph(captures, ["x", "none", "x"]) == [{0: "x"}, None, {0: "x"}]


def test_regex_operators_invalidate_and_revalidate_with_required_subjects():
    @hg.graph
    def matches(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches("x", text)

    @hg.graph
    def replace(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", "y", text)

    values = ["x", hg.INVALID, "z"]
    assert hg.run_graph(matches, values) == [True, None, False]
    assert hg.run_graph(replace, values) == ["y", None, "z"]


def test_regex_rejects_surrogates_before_utf8_conversion():
    @hg.graph
    def malformed_pattern(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches("\ud800", text)

    @hg.graph
    def malformed_replacement(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", "\ud800", text)

    @hg.graph
    def malformed_subject(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches("x", text)

    with pytest.raises(UnicodeEncodeError):
        hg.run_graph(malformed_pattern, ["x"])
    with pytest.raises(UnicodeEncodeError):
        hg.run_graph(malformed_replacement, ["x"])
    with pytest.raises(UnicodeEncodeError):
        hg.run_graph(malformed_subject, ["\ud800"])


def test_regex_pattern_and_replacement_errors_precede_search():
    @hg.graph
    def invalid_pattern(text: hg.TS[str]) -> hg.TS[bool]:
        return hg.operators.matches("(", text)

    @hg.graph
    def invalid_group(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\g<2>", text)

    @hg.graph
    def invalid_octal(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\400", text)

    @hg.graph
    def invalid_escape(text: hg.TS[str]) -> hg.TS[str]:
        return hg.operators.replace("x", r"\q", text)

    with pytest.raises(ValueError, match="regular expression"):
        hg.run_graph(invalid_pattern, ["no match"])
    with pytest.raises(ValueError, match="invalid group reference"):
        hg.run_graph(invalid_group, ["no match"])
    with pytest.raises(ValueError, match="octal escape"):
        hg.run_graph(invalid_octal, ["no match"])
    with pytest.raises(ValueError, match="bad escape"):
        hg.run_graph(invalid_escape, ["no match"])


def test_string_operations_invalidate_and_revalidate_with_required_inputs():
    @hg.graph
    def result_valid(value: hg.TS[str], count: hg.TS[int]) -> hg.TS[bool]:
        return hg.operators.valid(value * count)

    assert hg.run_graph(result_valid, [None, hg.INVALID, "ab", None, hg.INVALID, "cd"], [2] * 6) == [
        False,
        None,
        True,
        None,
        False,
        True,
    ]


def test_split_defaults_to_unlimited_and_preserves_embedded_bytes():
    @hg.graph
    def split(value: hg.TS[str]) -> hg.TS[tuple[str, ...]]:
        return hg.operators.split(value, "💠")

    assert hg.run_graph(split, ["a💠b💠c", "a\0💠é", "💠a💠"]) == [
        ("a", "b", "c"),
        ("a\0", "é"),
        ("", "a", ""),
    ]


def test_split_maxsplit_preserves_the_final_remainder():
    @hg.graph
    def split(value: hg.TS[str], maxsplit: hg.TS[int]) -> hg.TS[tuple[str, ...]]:
        return hg.operators.split(value, ",", maxsplit)

    assert hg.run_graph(split, ["a,b,c", "a,b,c", "a,b,c"], [0, 1, -1]) == [
        ("a,b,c",),
        ("a", "b,c"),
        ("a", "b", "c"),
    ]


def test_split_accepts_an_explicit_fixed_tsl_result():
    @hg.graph
    def split(value: hg.TS[str]) -> hg.TSL[hg.TS[str], 2]:
        return hg.operators.split[hg.TSL[hg.TS[str], 2]](value, ",")

    assert hg.run_graph(split, ["a,b", "x,y,z"]) == [
        {0: "a", 1: "b"},
        {0: "x", 1: "y,z"},
    ]

    with pytest.raises(ValueError, match="fewer pieces"):
        hg.run_graph(split, ["a"])


def test_join_accepts_scalar_lists_and_fixed_time_series_lists():
    @hg.graph
    def join_list(values: hg.TS[tuple[str, ...]]) -> hg.TS[str]:
        return hg.operators.join(values, "|")

    @hg.graph
    def join_tsl(values: hg.TSL[hg.TS[str], 2]) -> hg.TS[str]:
        return hg.operators.join(values, "💠")

    assert hg.run_graph(join_list, [("a", "b", "c"), (), ("é", "x\0y")]) == [
        "a|b|c",
        "",
        "é|x\0y",
    ]
    assert hg.run_graph(join_tsl, [{0: "a", 1: "b"}, {0: "x", 1: "y"}]) == [
        "a💠b",
        "x💠y",
    ]


def test_optional_join_skips_invalid_items_and_treats_an_invalid_root_as_empty():
    @hg.graph
    def join(values: hg.TSL[hg.TS[str], 2]) -> hg.TS[str]:
        return hg.operators.join(hg.opt(values), "|")

    assert hg.run_graph(join, [{0: "a", 1: hg.INVALID}, hg.INVALID, {0: "x", 1: "y"}]) == [
        "a",
        "",
        "x|y",
    ]


def test_str_renders_nested_scalar_lists_and_builtin_values():
    @hg.graph
    def render(values: hg.TS[tuple[int, ...]]) -> hg.TS[str]:
        return hg.operators.str_(values)

    @hg.graph
    def render_boolean(value: hg.TS[bool]) -> hg.TS[str]:
        return hg.operators.str_(value)

    assert hg.run_graph(render, [(1, 2), ()]) == ["[1, 2]", "[]"]
    assert hg.run_graph(render_boolean, [True, False]) == ["True", "False"]


def test_format_uses_positional_and_named_native_format_specs():
    @hg.graph
    def positional(value: hg.TS[int], ratio: hg.TS[float]) -> hg.TS[str]:
        return hg.operators.format_("{:04d} {:.2f}", value, ratio)

    @hg.graph
    def named(value: hg.TS[float]) -> hg.TS[str]:
        return hg.operators.format_("{value:.2f}", value=value)

    @hg.graph
    def no_arguments() -> hg.TS[str]:
        return hg.operators.format_("static\0text")

    assert hg.run_graph(positional, [7, -12], [1.25, 2.5]) == ["0007 1.25", "-012 2.50"]
    assert hg.run_graph(named, [1.25, 2.5]) == ["1.25", "2.50"]
    assert hg.run_graph(no_arguments) == ["static\0text"]


def test_print_writes_scalar_and_time_series_patterns_to_selected_stream(capfd):
    @hg.graph
    def emit(value: hg.TS[int], pattern: hg.TS[str]) -> hg.TS[int]:
        hg.operators.print_("value={:d}", value)
        hg.operators.print_(pattern, value)
        hg.operators.print_("error\0{:d}", value, __std_err__=True)
        return hg.operators.const_(0)

    assert hg.run_graph(emit, [7, 8], ["first={:d}", "second={:d}"]) == [0, None]
    captured = capfd.readouterr()
    assert captured.out == "value=7\nfirst=7\nvalue=8\nsecond=8\n"
    assert captured.err == "error\0" + "7\n" + "error\0" + "8\n"

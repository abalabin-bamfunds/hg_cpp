import bg


def test_boolean_reductions_accept_named_values_for_tsd():
    @bg.graph
    def all_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.all_(values=values)

    @bg.graph
    def any_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.any_(values=values)

    ticks = [{1: True, 2: True}, {1: False, 2: bg.REMOVE}, {1: bg.INVALID}, {1: bg.REMOVE}]
    assert bg.test_graph(all_values, ticks) == [True, False, None, True]
    assert bg.test_graph(any_values, ticks) == [True, False, None, False]


def test_optional_boolean_reductions_skip_invalid_items_and_use_empty_identity():
    @bg.graph
    def all_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.all_(values=bg.opt(values))

    @bg.graph
    def any_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.any_(values=bg.opt(values))

    ticks = [{1: True, 2: False}, {1: bg.REMOVE, 2: bg.INVALID}, {2: bg.REMOVE}, {3: True}, {3: bg.REMOVE}]
    assert bg.test_graph(all_values, ticks) == [False, True, True, True, True]
    assert bg.test_graph(any_values, ticks) == [True, False, False, True, False]


def test_boolean_reductions_support_boolean_sets():
    @bg.graph
    def all_values(values: bg.TSS[bool]) -> bg.TS[bool]:
        return bg.operators.all_(values=values)

    @bg.graph
    def any_values(values: bg.TSS[bool]) -> bg.TS[bool]:
        return bg.operators.any_(values=values)

    ticks = [set(), {False}, {True, bg.Removed(False)}, {bg.Removed(True)}]
    assert bg.test_graph(all_values, ticks) == [True, False, True, True]
    assert bg.test_graph(any_values, ticks) == [False, False, True, False]


def test_boolean_reductions_support_direct_fixed_lists_and_report_required_validity():
    @bg.graph
    def all_values(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.all_(values)

    @bg.graph
    def valid_all(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.valid(bg.operators.all_(values))

    @bg.graph
    def optional_any(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.any_(bg.opt(values))

    ticks = [None, {0: True}, {1: True}, {1: bg.INVALID}, {1: False}]
    assert bg.test_graph(all_values, ticks) == [None, None, True, None, False]
    assert bg.test_graph(valid_all, ticks) == [False, None, True, False, True]
    assert bg.test_graph(optional_any, ticks) == [None, True, True, True, True]


def test_optional_boolean_reductions_treat_invalid_fixed_list_as_empty():
    @bg.graph
    def all_values(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.all_(bg.opt(values))

    @bg.graph
    def any_values(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.any_(bg.opt(values))

    ticks = [None, {0: False}, {0: bg.INVALID}, {1: True}]
    assert bg.test_graph(all_values, ticks) == [None, False, True, True]
    assert bg.test_graph(any_values, ticks) == [None, False, False, True]


def test_boolean_reductions_support_valid_items_with_and_without_optional_root():
    @bg.graph
    def all_valid_items(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.all_(bg.valid_items(values))

    @bg.graph
    def any_optional_valid_items(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.any_(bg.opt(bg.valid_items(values)))

    ticks = [
        {1: True, 2: bg.INVALID},
        {1: bg.REMOVE, 2: False},
        {2: bg.INVALID},
        {2: bg.REMOVE},
    ]
    assert bg.test_graph(all_valid_items, ticks) == [True, False, True, True]
    assert bg.test_graph(any_optional_valid_items, ticks) == [True, False, False, False]


def test_optional_boolean_tss_reductions_treat_invalid_selected_target_as_empty():
    @bg.graph
    def all_values(select_invalid: bg.TS[bool], populated: bg.TSS[bool], invalid: bg.TSS[bool]) -> bg.TS[bool]:
        values = bg.operators.if_then_else(select_invalid, invalid, populated)
        return bg.operators.all_(bg.opt(values))

    @bg.graph
    def any_values(select_invalid: bg.TS[bool], populated: bg.TSS[bool], invalid: bg.TSS[bool]) -> bg.TS[bool]:
        values = bg.operators.if_then_else(select_invalid, invalid, populated)
        return bg.operators.any_(bg.opt(values))

    assert bg.test_graph(all_values, [False, True, False], [{False}, None, None], [None, None, None]) == [
        False,
        True,
        False,
    ]
    assert bg.test_graph(any_values, [False, True, False], [{True}, None, None], [None, None, None]) == [
        True,
        False,
        True,
    ]

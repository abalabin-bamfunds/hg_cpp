import bg


def test_boolean_reductions_accept_named_values_for_tsd():
    @bg.graph
    def all_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.all_(values=values)

    @bg.graph
    def any_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.any_(values=values)

    ticks = [{1: True, 2: True}, {1: False, 2: bg.REMOVE}, {1: bg.INVALID}, {1: bg.REMOVE}]
    assert bg.run_graph(all_values, ticks) == [True, False, None, True]
    assert bg.run_graph(any_values, ticks) == [True, False, None, False]


def test_optional_boolean_reductions_skip_invalid_items_and_use_empty_identity():
    @bg.graph
    def all_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.all_(values=bg.opt(values))

    @bg.graph
    def any_values(values: bg.TSD[int, bg.TS[bool]]) -> bg.TS[bool]:
        return bg.operators.any_(values=bg.opt(values))

    ticks = [{1: True, 2: False}, {1: bg.REMOVE, 2: bg.INVALID}, {2: bg.REMOVE}, {3: True}, {3: bg.REMOVE}]
    assert bg.run_graph(all_values, ticks) == [False, True, True, True, True]
    assert bg.run_graph(any_values, ticks) == [True, False, False, True, False]


def test_boolean_reductions_support_boolean_sets():
    @bg.graph
    def all_values(values: bg.TSS[bool]) -> bg.TS[bool]:
        return bg.operators.all_(values=values)

    @bg.graph
    def any_values(values: bg.TSS[bool]) -> bg.TS[bool]:
        return bg.operators.any_(values=values)

    ticks = [set(), {False}, {True, bg.Removed(False)}, {bg.Removed(True)}]
    assert bg.run_graph(all_values, ticks) == [True, False, True, True]
    assert bg.run_graph(any_values, ticks) == [False, False, True, False]


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
    assert bg.run_graph(all_values, ticks) == [None, None, True, None, False]
    assert bg.run_graph(valid_all, ticks) == [False, None, True, False, True]
    assert bg.run_graph(optional_any, ticks) == [None, True, True, True, True]


def test_optional_boolean_reductions_treat_invalid_fixed_list_as_empty():
    @bg.graph
    def all_values(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.all_(bg.opt(values))

    @bg.graph
    def any_values(values: bg.TSL[bg.TS[bool], 2]) -> bg.TS[bool]:
        return bg.operators.any_(bg.opt(values))

    ticks = [None, {0: False}, {0: bg.INVALID}, {1: True}]
    assert bg.run_graph(all_values, ticks) == [None, False, True, True]
    assert bg.run_graph(any_values, ticks) == [None, False, False, True]


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
    assert bg.run_graph(all_valid_items, ticks) == [True, False, True, True]
    assert bg.run_graph(any_optional_valid_items, ticks) == [True, False, False, False]


def test_optional_boolean_tss_reductions_treat_invalid_selected_target_as_empty():
    @bg.graph
    def all_values(select_invalid: bg.TS[bool], populated: bg.TSS[bool], invalid: bg.TSS[bool]) -> bg.TS[bool]:
        values = bg.operators.if_then_else(select_invalid, invalid, populated)
        return bg.operators.all_(bg.opt(values))

    @bg.graph
    def any_values(select_invalid: bg.TS[bool], populated: bg.TSS[bool], invalid: bg.TSS[bool]) -> bg.TS[bool]:
        values = bg.operators.if_then_else(select_invalid, invalid, populated)
        return bg.operators.any_(bg.opt(values))

    assert bg.run_graph(all_values, [False, True, False], [{False}, None, None], [None, None, None]) == [
        False,
        True,
        False,
    ]
    assert bg.run_graph(any_values, [False, True, False], [{True}, None, None], [None, None, None]) == [
        True,
        False,
        True,
    ]


def test_keys_where_true_selects_dictionary_values():
    @bg.compute_node
    def make_values(trigger: bg.TS[int]) -> bg.TSD[int, bg.TS[int]]:
        return {1: trigger.value * 10, 2: trigger.value * 20}

    @bg.compute_node
    def make_mask(value: bg.TS[bool]) -> bg.TSD[int, bg.TS[bool]]:
        return {1: value.value, 2: not value.value}

    @bg.graph
    def selected(trigger: bg.TS[int], condition: bg.TS[bool]) -> bg.TS[int]:
        values = make_values(trigger)
        mask = make_mask(condition)
        return values[bg.operators.keys_where_true(mask)][1]

    assert bg.run_graph(selected, [1, 2, 3], [True, False, True]) == [10, None, 30]


def test_keys_projects_dictionary_membership_without_item_value_updates():
    @bg.graph
    def key_set(values: bg.TSD[int, bg.TS[int]]) -> bg.TSS[int]:
        return bg.operators.keys_(dictionary=values)

    assert bg.run_graph(
        key_set,
        [
            {},
            {1: 10, 2: 20},
            {1: 11},
            {3: 30, 2: bg.REMOVE},
            {1: bg.REMOVE, 3: bg.REMOVE},
        ],
    ) == [
        bg.SetDelta(),
        bg.SetDelta(added={1, 2}),
        None,
        bg.SetDelta(added={3}, removed={2}),
        bg.SetDelta(removed={1, 3}),
    ]

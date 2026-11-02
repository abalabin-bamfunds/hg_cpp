import hg


def test_boolean_reductions_accept_named_values_for_tsd():
    @hg.graph
    def all_values(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.all_(values=values)

    @hg.graph
    def any_values(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.any_(values=values)

    ticks = [{1: True, 2: True}, {1: False, 2: hg.REMOVE}, {1: hg.INVALID}, {1: hg.REMOVE}]
    assert hg.run_graph(all_values, ticks) == [True, False, None, True]
    assert hg.run_graph(any_values, ticks) == [True, False, None, False]


def test_optional_boolean_reductions_skip_invalid_items_and_use_empty_identity():
    @hg.graph
    def all_values(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.all_(values=hg.opt(values))

    @hg.graph
    def any_values(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.any_(values=hg.opt(values))

    ticks = [{1: True, 2: False}, {1: hg.REMOVE, 2: hg.INVALID}, {2: hg.REMOVE}, {3: True}, {3: hg.REMOVE}]
    assert hg.run_graph(all_values, ticks) == [False, True, True, True, True]
    assert hg.run_graph(any_values, ticks) == [True, False, False, True, False]


def test_boolean_reductions_support_boolean_sets():
    @hg.graph
    def all_values(values: hg.TSS[bool]) -> hg.TS[bool]:
        return hg.operators.all_(values=values)

    @hg.graph
    def any_values(values: hg.TSS[bool]) -> hg.TS[bool]:
        return hg.operators.any_(values=values)

    ticks = [set(), {False}, {True, hg.Removed(False)}, {hg.Removed(True)}]
    assert hg.run_graph(all_values, ticks) == [True, False, True, True]
    assert hg.run_graph(any_values, ticks) == [False, False, True, False]


def test_boolean_reductions_support_direct_fixed_lists_and_report_required_validity():
    @hg.graph
    def all_values(values: hg.TSL[hg.TS[bool], 2]) -> hg.TS[bool]:
        return hg.operators.all_(values)

    @hg.graph
    def valid_all(values: hg.TSL[hg.TS[bool], 2]) -> hg.TS[bool]:
        return hg.operators.valid(hg.operators.all_(values))

    @hg.graph
    def optional_any(values: hg.TSL[hg.TS[bool], 2]) -> hg.TS[bool]:
        return hg.operators.any_(hg.opt(values))

    ticks = [None, {0: True}, {1: True}, {1: hg.INVALID}, {1: False}]
    assert hg.run_graph(all_values, ticks) == [None, None, True, None, False]
    assert hg.run_graph(valid_all, ticks) == [False, None, True, False, True]
    assert hg.run_graph(optional_any, ticks) == [None, True, True, True, True]


def test_optional_boolean_reductions_treat_invalid_fixed_list_as_empty():
    @hg.graph
    def all_values(values: hg.TSL[hg.TS[bool], 2]) -> hg.TS[bool]:
        return hg.operators.all_(hg.opt(values))

    @hg.graph
    def any_values(values: hg.TSL[hg.TS[bool], 2]) -> hg.TS[bool]:
        return hg.operators.any_(hg.opt(values))

    ticks = [None, {0: False}, {0: hg.INVALID}, {1: True}]
    assert hg.run_graph(all_values, ticks) == [None, False, True, True]
    assert hg.run_graph(any_values, ticks) == [None, False, False, True]


def test_boolean_reductions_support_valid_items_with_and_without_optional_root():
    @hg.graph
    def all_valid_items(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.all_(hg.valid_items(values))

    @hg.graph
    def any_optional_valid_items(values: hg.TSD[int, hg.TS[bool]]) -> hg.TS[bool]:
        return hg.operators.any_(hg.opt(hg.valid_items(values)))

    ticks = [
        {1: True, 2: hg.INVALID},
        {1: hg.REMOVE, 2: False},
        {2: hg.INVALID},
        {2: hg.REMOVE},
    ]
    assert hg.run_graph(all_valid_items, ticks) == [True, False, True, True]
    assert hg.run_graph(any_optional_valid_items, ticks) == [True, False, False, False]


def test_optional_boolean_tss_reductions_treat_invalid_selected_target_as_empty():
    @hg.graph
    def all_values(select_invalid: hg.TS[bool], populated: hg.TSS[bool], invalid: hg.TSS[bool]) -> hg.TS[bool]:
        values = hg.operators.if_then_else(select_invalid, invalid, populated)
        return hg.operators.all_(hg.opt(values))

    @hg.graph
    def any_values(select_invalid: hg.TS[bool], populated: hg.TSS[bool], invalid: hg.TSS[bool]) -> hg.TS[bool]:
        values = hg.operators.if_then_else(select_invalid, invalid, populated)
        return hg.operators.any_(hg.opt(values))

    assert hg.run_graph(all_values, [False, True, False], [{False}, None, None], [None, None, None]) == [
        False,
        True,
        False,
    ]
    assert hg.run_graph(any_values, [False, True, False], [{True}, None, None], [None, None, None]) == [
        True,
        False,
        True,
    ]


def test_keys_where_true_selects_dictionary_values():
    @hg.compute_node
    def make_values(trigger: hg.TS[int]) -> hg.TSD[int, hg.TS[int]]:
        return {1: trigger.value * 10, 2: trigger.value * 20}

    @hg.compute_node
    def make_mask(value: hg.TS[bool]) -> hg.TSD[int, hg.TS[bool]]:
        return {1: value.value, 2: not value.value}

    @hg.graph
    def selected(trigger: hg.TS[int], condition: hg.TS[bool]) -> hg.TS[int]:
        values = make_values(trigger)
        mask = make_mask(condition)
        return values[hg.operators.keys_where_true(mask)][1]

    assert hg.run_graph(selected, [1, 2, 3], [True, False, True]) == [10, None, 30]


def test_keys_projects_dictionary_membership_without_item_value_updates():
    @hg.graph
    def key_set(values: hg.TSD[int, hg.TS[int]]) -> hg.TSS[int]:
        return hg.operators.keys_(dictionary=values)

    assert hg.run_graph(
        key_set,
        [
            {},
            {1: 10, 2: 20},
            {1: 11},
            {3: 30, 2: hg.REMOVE},
            {1: hg.REMOVE, 3: hg.REMOVE},
        ],
    ) == [
        hg.SetDelta(),
        hg.SetDelta(added={1, 2}),
        None,
        hg.SetDelta(added={3}, removed={2}),
        hg.SetDelta(removed={1, 3}),
    ]

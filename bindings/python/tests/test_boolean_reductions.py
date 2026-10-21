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

    ticks = [set(), {False}, bg.SetDelta(added={True}, removed={False}), bg.SetDelta(removed={True})]
    assert bg.test_graph(all_values, ticks) == [True, False, True, True]
    assert bg.test_graph(any_values, ticks) == [False, False, True, False]


def test_boolean_reductions_support_fixed_lists_and_nested_operator_results():
    @bg.compute_node
    def make_list(trigger: bg.TS[int]) -> bg.TSL[bg.TS[bool], 2]:
        if trigger.value == 0:
            return {0: True, 1: True}
        if trigger.value == 1:
            return {0: False, 1: False}
        return {0: False, 1: True}

    @bg.graph
    def all_values(trigger: bg.TS[int]) -> bg.TS[bool]:
        return bg.operators.all_(values=make_list(trigger))

    @bg.graph
    def optional_any(trigger: bg.TS[int]) -> bg.TS[bool]:
        values = bg.opt(make_list(trigger))
        return bg.operators.not_(bg.operators.any_(values=values))

    assert bg.test_graph(all_values, [0, 1, 2]) == [True, False, False]
    assert bg.test_graph(optional_any, [0, 1, 2]) == [False, True, False]

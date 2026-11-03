import hgraph as hg


def test_scalar_set_operators_publish_complete_results():
    @hg.graph
    def union(lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return lhs | rhs

    @hg.graph
    def intersection(lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return lhs & rhs

    @hg.graph
    def difference(lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return lhs - rhs

    @hg.graph
    def symmetric_difference(
        lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]
    ) -> hg.TS[frozenset[int]]:
        return lhs ^ rhs

    lhs = [{1, 2}, {1}, hg.INVALID, {3}, {3}]
    rhs = [{2, 3}, {1, 4}, {2}, {3, 4}, {3, 4}]

    assert hg.run_graph(union, lhs, rhs) == [{1, 2, 3}, {1, 4}, None, {3, 4}, {3, 4}]
    assert hg.run_graph(intersection, lhs, rhs) == [{2}, {1}, None, {3}, {3}]
    assert hg.run_graph(difference, lhs, rhs) == [{1}, set(), None, set(), set()]
    assert hg.run_graph(symmetric_difference, lhs, rhs) == [{1, 3}, {4}, None, {4}, {4}]

    @hg.graph
    def string_union(lhs: hg.TS[frozenset[str]], rhs: hg.TS[frozenset[str]]) -> hg.TS[frozenset[str]]:
        return lhs | rhs

    assert hg.run_graph(string_union, [{"a", "b"}], [{"b", "c"}]) == [{"a", "b", "c"}]


def test_optional_scalar_set_operators_treat_invalid_inputs_as_empty():
    @hg.graph
    def union(lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        return hg.operators.bit_or(hg.opt(lhs), rhs)

    assert hg.run_graph(union, [{1}, hg.INVALID, {2}], [{2}, {3}, hg.INVALID]) == [
        {1, 2},
        {3},
        {2},
    ]


def test_scalar_list_and_set_numeric_reductions_include_empty_values():
    @hg.graph
    def list_sum(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.sum_(values)

    @hg.graph
    def set_sum(values: hg.TS[frozenset[int]]) -> hg.TS[int]:
        return hg.operators.sum_(values)

    @hg.graph
    def list_min(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.min_(values)

    @hg.graph
    def set_max(values: hg.TS[frozenset[int]]) -> hg.TS[int]:
        return hg.operators.max_(values)

    @hg.graph
    def list_mean(values: hg.TS[tuple[int, ...]]) -> hg.TS[float]:
        return hg.operators.mean(values)

    @hg.graph
    def optional_list_min(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.min_(hg.opt(values))

    @hg.graph
    def optional_set_max(values: hg.TS[frozenset[int]]) -> hg.TS[int]:
        return hg.operators.max_(hg.opt(values))

    @hg.graph
    def optional_list_mean(values: hg.TS[tuple[int, ...]]) -> hg.TS[float]:
        return hg.operators.mean(hg.opt(values))

    values = [(1, 2, 3), (), (4, 5)]
    assert hg.run_graph(list_sum, values) == [6, 0, 9]
    assert hg.run_graph(set_sum, [{1, 2, 3}, set(), {4, 5}]) == [6, 0, 9]
    assert hg.run_graph(list_min, values) == [1, None, 4]
    assert hg.run_graph(set_max, [{1, 2, 3}, set(), {4, 5}]) == [3, None, 5]
    assert hg.run_graph(list_mean, values) == [2.0, None, 4.5]
    optional_values = [(), hg.INVALID, (4,)]
    assert hg.run_graph(optional_list_min, optional_values) == [None, None, 4]
    assert hg.run_graph(optional_list_mean, optional_values) == [None, None, 4.0]
    assert hg.run_graph(optional_set_max, [set(), hg.INVALID, {4}]) == [None, None, 4]


def test_scalar_list_and_set_double_reductions():
    @hg.graph
    def list_sum(values: hg.TS[tuple[float, ...]]) -> hg.TS[float]:
        return hg.operators.sum_(values)

    @hg.graph
    def list_min(values: hg.TS[tuple[float, ...]]) -> hg.TS[float]:
        return hg.operators.min_(values)

    @hg.graph
    def list_max(values: hg.TS[tuple[float, ...]]) -> hg.TS[float]:
        return hg.operators.max_(values)

    @hg.graph
    def list_mean(values: hg.TS[tuple[float, ...]]) -> hg.TS[float]:
        return hg.operators.mean(values)

    @hg.graph
    def set_sum(values: hg.TS[frozenset[float]]) -> hg.TS[float]:
        return hg.operators.sum_(values)

    @hg.graph
    def set_min(values: hg.TS[frozenset[float]]) -> hg.TS[float]:
        return hg.operators.min_(values)

    @hg.graph
    def set_max(values: hg.TS[frozenset[float]]) -> hg.TS[float]:
        return hg.operators.max_(values)

    @hg.graph
    def set_mean(values: hg.TS[frozenset[float]]) -> hg.TS[float]:
        return hg.operators.mean(values)

    list_values = [(1.5, 2.5), (), (-3.0, 4.0)]
    set_values = [{1.5, 2.5}, set(), {-3.0, 4.0}]
    assert hg.run_graph(list_sum, list_values) == [4.0, 0.0, 1.0]
    assert hg.run_graph(list_min, list_values) == [1.5, None, -3.0]
    assert hg.run_graph(list_max, list_values) == [2.5, None, 4.0]
    assert hg.run_graph(list_mean, list_values) == [2.0, None, 0.5]
    assert hg.run_graph(set_sum, set_values) == [4.0, 0.0, 1.0]
    assert hg.run_graph(set_min, set_values) == [1.5, None, -3.0]
    assert hg.run_graph(set_max, set_values) == [2.5, None, 4.0]
    assert hg.run_graph(set_mean, set_values) == [2.0, None, 0.5]


def test_scalar_list_and_set_boolean_reductions_use_empty_identities():
    @hg.graph
    def all_list(values: hg.TS[tuple[bool, ...]]) -> hg.TS[bool]:
        return hg.operators.all_(values)

    @hg.graph
    def any_list(values: hg.TS[tuple[bool, ...]]) -> hg.TS[bool]:
        return hg.operators.any_(values)

    @hg.graph
    def all_set(values: hg.TS[frozenset[bool]]) -> hg.TS[bool]:
        return hg.operators.all_(values)

    @hg.graph
    def optional_any(values: hg.TS[frozenset[bool]]) -> hg.TS[bool]:
        return hg.operators.any_(hg.opt(values))

    @hg.graph
    def optional_all_list(values: hg.TS[tuple[bool, ...]]) -> hg.TS[bool]:
        return hg.operators.all_(hg.opt(values))

    @hg.graph
    def optional_any_list(values: hg.TS[tuple[bool, ...]]) -> hg.TS[bool]:
        return hg.operators.any_(hg.opt(values))

    @hg.graph
    def optional_all_set(values: hg.TS[frozenset[bool]]) -> hg.TS[bool]:
        return hg.operators.all_(hg.opt(values))

    values = [(True, False), (), (True,), hg.INVALID]
    set_values = [{True, False}, set(), {True}, hg.INVALID]

    assert hg.run_graph(all_list, values) == [False, True, True, None]
    assert hg.run_graph(any_list, values) == [True, False, True, None]
    assert hg.run_graph(all_set, set_values) == [False, True, True, None]
    assert hg.run_graph(optional_any, set_values) == [True, False, True, False]
    assert hg.run_graph(optional_all_list, values) == [False, True, True, True]
    assert hg.run_graph(optional_any_list, values) == [True, False, True, False]
    assert hg.run_graph(optional_all_set, set_values) == [False, True, True, True]


def test_scalar_set_operations_follow_reference_retargeting():
    @hg.graph
    def union(select_lhs: hg.TS[bool], lhs: hg.TS[frozenset[int]], rhs: hg.TS[frozenset[int]]) -> hg.TS[frozenset[int]]:
        selected = hg.operators.if_then_else(select_lhs, lhs, rhs)
        return hg.operators.bit_or(lhs, selected)

    assert hg.run_graph(union, [False, True, False], [{1}, {1, 3}, {2}], [{2}, {4}, {2, 4}]) == [
        {1, 2},
        {1, 3},
        {2, 4},
    ]


def test_scalar_list_and_set_integer_sums_wrap_at_i64_width():
    @hg.graph
    def list_sum(values: hg.TS[tuple[int, ...]]) -> hg.TS[int]:
        return hg.operators.sum_(values)

    @hg.graph
    def set_sum(values: hg.TS[frozenset[int]]) -> hg.TS[int]:
        return hg.operators.sum_(values)

    largest_i64 = (1 << 63) - 1
    smallest_i64 = -(1 << 63)
    assert hg.run_graph(list_sum, [(largest_i64, 1)]) == [smallest_i64]
    assert hg.run_graph(set_sum, [{largest_i64, 1}]) == [smallest_i64]

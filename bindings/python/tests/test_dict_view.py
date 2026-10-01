from collections.abc import Mapping

import pytest

import bg


def test_plain_node_receives_a_lazy_mapping_for_a_dictionary_time_series():
    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return {1: 10, 2: 20, 3: 30}

    @bg.node
    def inspect(values: dict[int, int]) -> int:
        assert isinstance(values, Mapping)
        assert isinstance(values, bg.Dict)
        assert len(values) == 3
        assert values[2] == 20
        assert values.get(4, 40) == 40
        assert 3 in values
        assert 4 not in values

        key_view = values.keys()
        item_view = values.items()
        value_view = values.values()

        assert len(key_view) == len(item_view) == len(value_view) == 3
        assert set(key_view) == {1, 2, 3}
        assert set(item_view) == {(1, 10), (2, 20), (3, 30)}
        assert set(value_view) == {10, 20, 30}
        assert values.copy() == {1: 10, 2: 20, 3: 30}
        assert dict(values) == {1: 10, 2: 20, 3: 30}
        return values[1]

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [10]


def test_dictionary_view_raises_key_error_and_is_read_only():
    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return {1: 10}

    @bg.node
    def inspect(values: dict[int, int]) -> int:
        with pytest.raises(KeyError) as error:
            values[2]
        assert error.value.args == (2,)
        with pytest.raises(TypeError):
            values[2] = 20
        return len(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [1]


def test_dictionary_view_expires_when_the_node_callback_returns():
    saved = []

    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return {1: 10}

    @bg.node
    def retain(values: dict[int, int]) -> int:
        saved.append(values)
        saved.append(iter(values))
        saved.append(iter(values.keys()))
        saved.append(values.keys())
        saved.append(values.items())
        saved.append(iter(values.items()))
        saved.append(values.values())
        saved.append(iter(values.values()))
        return values[1]

    @bg.graph
    def graph() -> bg.TS[int]:
        return retain(source())

    assert bg.test_graph(graph) == [10]
    with pytest.raises(RuntimeError, match="outside its callback"):
        len(saved[0])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(saved[1])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(saved[2])
    with pytest.raises(RuntimeError, match="outside its callback"):
        len(saved[3])
    with pytest.raises(RuntimeError, match="outside its callback"):
        len(saved[4])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(saved[5])
    with pytest.raises(RuntimeError, match="outside its callback"):
        len(saved[6])
    with pytest.raises(RuntimeError, match="outside its callback"):
        next(saved[7])


def test_dictionary_cursor_iterators_cross_batches_and_views_are_reiterable():
    expected = {index: index * 3 for index in range(130)}

    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return expected

    @bg.node
    def inspect(values: dict[int, int]) -> int:
        assert list(values) == list(expected)
        assert list(values) == list(expected)
        assert list(values.keys()) == list(expected)
        assert list(values.values()) == list(expected.values())
        assert list(values.values()) == list(expected.values())
        assert list(values.items()) == list(expected.items())
        assert list(values.items()) == list(expected.items())
        return len(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [130]


def test_empty_dictionary_cursor_iterators_stop_immediately():
    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return {}

    @bg.node
    def inspect(values: dict[int, int]) -> int:
        assert list(values) == []
        assert list(values.keys()) == []
        assert list(values.values()) == []
        assert list(values.items()) == []
        return len(values)

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect(source())

    assert bg.test_graph(graph) == [0]


def test_dictionary_time_series_output_still_materializes_as_a_python_dict():
    @bg.source_node
    def source() -> bg.TS[dict[int, int]]:
        return {1: 10, 2: 20}

    @bg.graph
    def graph() -> bg.TS[dict[int, int]]:
        return source()

    assert bg.test_graph(graph) == [{1: 10, 2: 20}]


def test_scalar_dictionary_arguments_use_callback_scoped_views():
    saved = []

    @bg.node
    def inspect(values: dict[str, int]) -> int:
        assert isinstance(values, bg.Dict)
        saved.append(values)
        return values["answer"]

    @bg.graph
    def graph() -> bg.TS[int]:
        return inspect({"answer": 42})

    assert bg.test_graph(graph) == [42]
    with pytest.raises(RuntimeError, match="outside its callback"):
        len(saved[0])

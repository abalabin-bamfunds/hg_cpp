import hgraph as hg
from hgraph._hg_test_extension import compiled_wiring_paths, custom_labeled_source


@hg.compute_node
def identity(value: hg.TS[int]) -> hg.TS[int]:
    return value.value


@hg.source_node
def source() -> hg.TS[int]:
    return 1


def _paths(graph):
    return compiled_wiring_paths(graph._native_definition)


def test_assigned_python_node_output_uses_local_name():
    @hg.graph
    def graph() -> hg.TS[int]:
        result = source()
        return result

    assert any(path.endswith("/source[result]") for path in _paths(graph))


def test_unassigned_python_node_output_keeps_generated_label():
    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    source_path = next(path for path in _paths(graph) if "/source[" in path)
    label = source_path.rsplit("[", 1)[1][:-1]
    assert label.isdigit()


def test_native_operator_output_uses_local_name():
    @hg.graph
    def graph() -> hg.TS[int]:
        value = source()
        result = hg.operators.add(value, 1)
        return result

    paths = _paths(graph)
    assert any(path.endswith("/add[result]") for path in paths)


def test_nested_graph_labels_its_node_from_its_local_name():
    @hg.graph
    def inner() -> hg.TS[int]:
        value = source()
        inner_result = identity(value)
        return inner_result

    @hg.graph
    def outer() -> hg.TS[int]:
        outer_result = inner()
        return outer_result

    paths = _paths(outer)
    assert any("/inner[" in path and path.endswith("/identity[inner_result]") for path in paths)


def test_deduplicated_node_keeps_first_local_name():
    @hg.graph
    def graph() -> hg.TS[int]:
        first = source()
        second = source()
        return second

    source_paths = [path for path in _paths(graph) if "/source[" in path]
    assert len(source_paths) == 1
    assert source_paths[0].endswith("/source[first]")


def test_reassigned_local_keeps_name_for_each_node_output():
    @hg.graph
    def graph() -> hg.TS[int]:
        result = source()
        result = identity(result)
        return result

    paths = _paths(graph)
    assert any(path.endswith("/source[result]") for path in paths)
    assert any(path.endswith("/identity[result]") for path in paths)


def test_repeated_call_site_labels_each_reassigned_output():
    @hg.graph
    def graph() -> hg.TS[int]:
        result = source()
        for _ in range(2):
            result = identity(result)
        return result

    identity_paths = [path for path in _paths(graph) if "/identity[" in path]
    assert len(identity_paths) == 2
    assert all(path.endswith("/identity[result]") for path in identity_paths)


def test_python_assignment_does_not_replace_explicit_native_label():
    @hg.graph
    def graph() -> hg.TS[int]:
        result = custom_labeled_source()
        return result

    assert any(path.endswith("/custom_labeled_source_definition[native-custom]") for path in _paths(graph))

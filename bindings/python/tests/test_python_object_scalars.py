import pytest

import bg
from bg._bg_test_extension import can_convert_python_type, copy_release_python_scalar_without_gil


def test_python_objects_keep_identity_through_base_typed_time_series():
    class Base:
        pass

    class Derived(Base):
        pass

    value = Derived()

    # Register the derived class first; its bases must still resolve to the same scalar types.
    @bg.graph
    def derived_to_base(value: bg.TS[Derived]) -> bg.TS[Base]:
        return value

    @bg.graph
    def base_to_base(value: bg.TS[Base]) -> bg.TS[Base]:
        return value

    assert bg.test_graph(derived_to_base, [value])[0] is value
    assert bg.test_graph(base_to_base, [value])[0] is value


def test_python_object_scalars_support_multiple_inheritance():
    class Left:
        pass

    class Right:
        pass

    class Derived(Left, Right):
        pass

    value = Derived()

    @bg.graph
    def as_left(value: bg.TS[Derived]) -> bg.TS[Left]:
        return value

    @bg.graph
    def as_right(value: bg.TS[Derived]) -> bg.TS[Right]:
        return value

    assert bg.test_graph(as_left, [value])[0] is value
    assert bg.test_graph(as_right, [value])[0] is value


def test_object_annotation_accepts_a_previously_registered_python_class():
    class Value:
        pass

    assert can_convert_python_type(Value)

    @bg.graph
    def graph(value: bg.TS[object]) -> bg.TS[object]:
        return value

    value = Value()
    assert bg.test_graph(graph, [value])[0] is value


def test_can_from_python_recognizes_python_object_and_builtin_scalar_types():
    class Value:
        pass

    assert can_convert_python_type(Value)
    assert can_convert_python_type(int)


def test_python_object_constants_copy_without_changing_identity():
    class Value:
        pass

    value = Value()

    @bg.graph
    def graph() -> bg.TS[Value]:
        return bg.operators.const_(value)

    assert bg.test_graph(graph)[0] is value


def test_python_object_reference_copy_and_final_release_do_not_need_the_gil():
    class Value:
        pass

    value = Value()
    assert copy_release_python_scalar_without_gil(value)
    assert type(value) is Value


def test_python_node_returning_none_produces_no_tick_for_object_scalars():
    class Value:
        pass

    value = Value()

    @bg.compute_node
    def maybe(trigger: bg.TS[int]) -> bg.TS[Value]:
        return value if trigger.value == 1 else None

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[Value]:
        return maybe(trigger)

    result = bg.test_graph(graph, [1, 2])
    assert result[0] is value
    assert result[1] is None


def test_none_is_not_a_python_object_scalar_value():
    @bg.graph
    def graph() -> bg.TS[type(None)]:
        return bg.operators.const_(None)

    with pytest.raises(ValueError, match="None does not identify a scalar type"):
        bg.test_graph(graph)


def test_tss_base_keys_use_python_hash_and_equality_across_classes():
    class Base:
        def __init__(self, value):
            self.value = value

        def __hash__(self):
            return hash(self.value)

        def __eq__(self, other):
            return isinstance(other, Base) and self.value == other.value

    class Derived(Base):
        pass

    base = Base(1)
    derived = Derived(1)

    @bg.compute_node
    def make_set(trigger: bg.TS[int]) -> bg.TSS[Base]:
        delta = bg.SetDelta()
        if trigger.value == 1:
            # SetDelta normally stores a frozenset. A list lets the engine apply
            # both candidates and exercise its own cross-class equality path.
            delta.added = [base, derived]
        else:
            delta.removed = [derived]
        return delta

    @bg.compute_node
    def inspect(value: bg.TSS[Base]) -> bg.TS[int]:
        if value.was_added(derived):
            return 1
        if value.was_removed(derived):
            return 2
        return len(value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_set(trigger))

    assert bg.test_graph(graph, [1, 2]) == [1, 2]


def test_tsd_base_keys_support_derived_lookup_and_removal():
    class Base:
        def __init__(self, value):
            self.value = value

        def __hash__(self):
            return hash(self.value)

        def __eq__(self, other):
            return isinstance(other, Base) and self.value == other.value

    class Derived(Base):
        pass

    base = Base(1)
    derived = Derived(1)

    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[Base, bg.TS[int]]:
        return {base: 10} if trigger.value == 1 else {derived: bg.REMOVE}

    @bg.compute_node
    def inspect(trigger: bg.TS[int], values: bg.TSD[Base, bg.TS[int]]) -> bg.TS[int]:
        return values[derived].value if derived in values else 0

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(trigger, make_dictionary(trigger))

    assert bg.test_graph(graph, [1, 2]) == [10, 0]


@pytest.mark.parametrize("operation", ["hash", "equality"])
def test_python_hash_and_equality_exceptions_keep_their_identity(operation):
    error = RuntimeError(f"{operation} failed")

    if operation == "hash":

        class Key:
            def __hash__(self):
                raise error

        added = [Key()]
    else:

        class Key:
            def __hash__(self):
                return 1

            def __eq__(self, other):
                raise error

        added = [Key(), Key()]

    @bg.compute_node
    def make_set(trigger: bg.TS[int]) -> bg.TSS[Key]:
        delta = bg.SetDelta()
        delta.added = added
        return delta

    @bg.compute_node
    def inspect(value: bg.TSS[Key]) -> bg.TS[int]:
        return len(value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_set(trigger))

    with pytest.raises(RuntimeError) as raised:
        bg.test_graph(graph, [1])

    assert raised.value is error


def test_unhashable_python_objects_raise_type_error_as_tss_keys():
    class Base:
        def __hash__(self):
            return 1

    class Key(Base):
        __hash__ = None

    key = Key()

    @bg.compute_node
    def make_set(trigger: bg.TS[int]) -> bg.TSS[Base]:
        delta = bg.SetDelta()
        delta.added = [key]
        return delta

    @bg.compute_node
    def inspect(value: bg.TSS[Base]) -> bg.TS[int]:
        return len(value)

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(make_set(trigger))

    with pytest.raises(TypeError, match="unhashable type"):
        bg.test_graph(graph, [1])


def test_unhashable_python_objects_raise_type_error_as_tsd_lookup_keys():
    class Base:
        def __init__(self, value):
            self.value = value

        def __hash__(self):
            return hash(self.value)

        def __eq__(self, other):
            return isinstance(other, Base) and self.value == other.value

    class Unhashable(Base):
        __hash__ = None

    stored = Base(1)
    missing = Unhashable(2)

    @bg.compute_node
    def make_dictionary(trigger: bg.TS[int]) -> bg.TSD[Base, bg.TS[int]]:
        return {stored: 10}

    @bg.compute_node
    def inspect(trigger: bg.TS[int], values: bg.TSD[Base, bg.TS[int]]) -> bg.TS[int]:
        return values[missing].value if missing in values else 0

    @bg.graph
    def graph(trigger: bg.TS[int]) -> bg.TS[int]:
        return inspect(trigger, make_dictionary(trigger))

    with pytest.raises(TypeError, match="unhashable type"):
        bg.test_graph(graph, [1])

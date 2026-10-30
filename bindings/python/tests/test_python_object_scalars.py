import pytest

import bg
from bg._bg_test_extension import (
    can_convert_python_type,
    copy_release_python_scalar_without_gil,
    native_concrete_type_name,
    python_type_supports_value_covariance,
)


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

    assert bg.run_graph(derived_to_base, [value])[0] is value
    assert bg.run_graph(base_to_base, [value])[0] is value


def test_python_object_defaults_resolve_inside_list_and_bundle_scalars():
    class Value:
        pass

    @bg.schema
    class Record:
        value: Value

    first = Value()
    second = Value()
    record = Record(first)

    @bg.graph
    def list_identity(value: bg.TS[tuple[Value, ...]]) -> bg.TS[tuple[Value, ...]]:
        return value

    @bg.graph
    def bundle_identity(value: bg.TS[bg.Bundle[Record]]) -> bg.TS[bg.Bundle[Record]]:
        return value

    assert bg.run_graph(list_identity, [(first, second)]) == [(first, second)]
    result = bg.run_graph(bundle_identity, [record])
    assert type(result[0]) is Record
    assert result[0].value is first


def test_python_object_scalar_types_report_value_covariance():
    class Base:
        pass

    class Derived(Base):
        pass

    assert python_type_supports_value_covariance(Base)
    assert python_type_supports_value_covariance(Derived)
    assert not python_type_supports_value_covariance(int)


def test_type_operator_reports_concrete_python_object_classes_from_base_typed_input():
    class Base:
        pass

    class DerivedA(Base):
        pass

    class DerivedB(Base):
        pass

    @bg.graph
    def graph(value: bg.TS[Base]) -> bg.TS[str]:
        return native_concrete_type_name(value)

    assert bg.run_graph(graph, [DerivedA(), DerivedA(), DerivedB(), Base()]) == [
        "DerivedA",
        "DerivedA",
        "DerivedB",
        "Base",
    ]


def test_type_operator_reports_concrete_dataclass_schema_from_base_typed_input():
    @bg.schema
    class BaseRecord:
        value: int

    @bg.schema
    class DerivedRecord(BaseRecord):
        extra: int

    @bg.graph
    def graph(value: bg.TS[bg.Bundle[BaseRecord]]) -> bg.TS[str]:
        return native_concrete_type_name(value)

    assert bg.run_graph(graph, [DerivedRecord(1, 2), DerivedRecord(2, 3), BaseRecord(4)]) == [
        "DerivedRecord",
        "DerivedRecord",
        "BaseRecord",
    ]


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

    assert bg.run_graph(as_left, [value])[0] is value
    assert bg.run_graph(as_right, [value])[0] is value


def test_object_annotation_accepts_a_previously_registered_python_class():
    class Value:
        pass

    assert can_convert_python_type(Value)

    @bg.graph
    def graph(value: bg.TS[object]) -> bg.TS[object]:
        return value

    value = Value()
    assert bg.run_graph(graph, bg.ticks([value]))[0] is value


def test_run_graph_uses_registered_object_types_to_classify_iterable_values():
    class Base:
        pass

    class Derived(Base):
        pass

    @bg.graph
    def base_values(value: bg.TS[Base]) -> bg.TS[Base]:
        return value

    @bg.graph
    def object_values(value: bg.TS[object]) -> bg.TS[object]:
        return value

    derived = [Derived(), Derived()]
    list_value = [1, 2]

    assert bg.run_graph(base_values, derived) == derived
    assert bg.run_graph(object_values, list_value) == [list_value]
    assert bg.run_graph(object_values, bg.ticks([list_value, [3, 4]])) == [list_value, [3, 4]]


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

    assert bg.run_graph(graph)[0] is value


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

    result = bg.run_graph(graph, [1, 2])
    assert result[0] is value
    assert result[1] is None


def test_none_is_not_a_python_object_scalar_value():
    @bg.graph
    def graph() -> bg.TS[type(None)]:
        return bg.operators.const_(None)

    with pytest.raises(ValueError, match="None does not identify a scalar type"):
        bg.run_graph(graph)


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

    assert bg.run_graph(graph, [1, 2]) == [1, 2]


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

    assert bg.run_graph(graph, [1, 2]) == [10, 0]


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
        bg.run_graph(graph, [1])

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
        bg.run_graph(graph, [1])


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
        bg.run_graph(graph, [1])

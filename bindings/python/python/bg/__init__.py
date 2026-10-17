"""Python graph authoring for the bg engine."""

from __future__ import annotations

import atexit
import inspect
import types
from datetime import datetime, timedelta
from functools import update_wrapper
from typing import Annotated, Any, Generic, Tuple, TypeVar, get_args, get_origin, get_type_hints
from weakref import WeakKeyDictionary

from . import operators
from ._bg import (
    EvaluationEngine,
    EvaluationClock,
    Queue,
    QueueMode,
    Dict,
    EvaluationProfiler,
    EvaluationProfilerRecorder as _EvaluationProfilerRecorder,
    StopSource,
    StopToken,
    Scheduler,
    ScheduleHandle,
    _TimeSeriesGuarantee,
    WiringPoint as _WiringPoint,
    _Type,
    _make_graph,
    _make_node,
    _make_schema,
    _register_python_schema,
    _dict_scalar_type,
    _list_scalar_type,
    _tuple_scalar_type,
    _map,
    _mesh,
    _mesh_dependency,
    _node_input_type,
    _optional_type,
    _reference_type,
    _register_delta_types,
    _register_symbol_type,
    _run_graph,
    _scalar_type,
    _scalar_variable_type,
    _test_graph,
    _make_service,
    _make_service_catalogue,
    _register_service_impl,
    _wire_service,
    _time_series_dict_type,
    _time_series_bundle_type,
    _time_series_list_type,
    _variadic_time_series_bundle_type,
    _variadic_time_series_list_type,
    _time_series_set_type,
    _time_series_type,
    _time_series_variable_type,
    _switch,
    _specialize_graph,
    _wire_graph,
    _wire_node,
    _with_time_series_guarantee,
    _with_valid_items,
    version,
)


class Symbol(str):
    """An interned engine symbol represented as Python text."""

    __slots__ = ("_is_null",)

    _instances: dict[bytes | None, Symbol] = {}

    def __new__(cls, value: str | None = None):
        is_null = value is None or (isinstance(value, Symbol) and value._is_null)
        text = "" if is_null else str(value)
        key = None if is_null else text.encode("utf-8")
        cached = cls._instances.get(key)
        if cached is not None:
            return cached

        result = super().__new__(cls, text)
        object.__setattr__(result, "_is_null", is_null)
        cls._instances[key] = result
        return result

    def __init_subclass__(cls, **kwargs: Any) -> None:
        raise TypeError("bg.Symbol cannot be subclassed")

    def __setattr__(self, name: str, value: Any) -> None:
        if name == "_is_null":
            raise AttributeError("bg.Symbol null state is immutable")
        object.__setattr__(self, name, value)

    def __delattr__(self, name: str) -> None:
        if name == "_is_null":
            raise AttributeError("bg.Symbol null state is immutable")
        object.__delattr__(self, name)

    def __str__(self) -> str:
        return str.__str__(self)

    def __repr__(self) -> str:
        if self._is_null:
            return "symbol()"
        return f"symbol({str.__repr__(self)})"

    def __eq__(self, other: Any) -> bool:
        if not isinstance(other, str):
            return NotImplemented
        if self._is_null:
            return isinstance(other, Symbol) and other._is_null
        if isinstance(other, Symbol) and other._is_null:
            return False
        return str.__eq__(self, other)

    def __ne__(self, other: Any) -> bool:
        result = self.__eq__(other)
        return NotImplemented if result is NotImplemented else not result

    def __hash__(self) -> int:
        return hash((Symbol, None)) if self._is_null else str.__hash__(self)


_register_symbol_type(Symbol)
symbol = Symbol


class _Invalid:
    def __repr__(self) -> str:
        return "INVALID"


INVALID = _Invalid()


class _Remove:
    def __repr__(self) -> str:
        return "REMOVE"


REMOVE = _Remove()


class SetDelta:
    __slots__ = ("added", "removed")

    def __init__(self, added: Any = (), removed: Any = ()):
        self.added = frozenset(added)
        self.removed = frozenset(removed)
        overlap = self.added & self.removed
        if overlap:
            raise ValueError(f"a set delta cannot add and remove the same values: {overlap!r}")

    def __repr__(self) -> str:
        return f"SetDelta(added={self.added!r}, removed={self.removed!r})"

    def __eq__(self, other: Any) -> bool:
        if not isinstance(other, SetDelta):
            return NotImplemented
        return self.added == other.added and self.removed == other.removed


class Removed:
    """Mark a TSS item for removal in a set or frozenset output delta."""

    __slots__ = ("item",)

    def __init__(self, item: Any):
        self.item = item

    def __hash__(self) -> int:
        return hash(self.item)

    def __eq__(self, other: Any) -> bool:
        return self.item == other.item if type(other) is Removed else self.item == other

    def __repr__(self) -> str:
        return f"Removed({self.item!r})"


_register_delta_types(SetDelta, Removed, REMOVE, INVALID)


ordinary = _TimeSeriesGuarantee.ordinary
deduplicated = _TimeSeriesGuarantee.deduplicated
final = _TimeSeriesGuarantee.final
const_ = _TimeSeriesGuarantee.const_


class _ValidItems:
    def __repr__(self) -> str:
        return "bg.valid_items"

    def __call__(self, point: Any) -> Any:
        if not isinstance(point, _WiringPoint):
            raise TypeError("valid_items expects a time-series wiring point")
        return point._as_valid_items()


valid_items = _ValidItems()


class TimeSeries:
    """Typing marker for TypeVars that represent any time-series type."""


_SCALAR_T = TypeVar("_SCALAR_T")
_TIME_SERIES_T = TypeVar("_TIME_SERIES_T", bound=TimeSeries)
_ITEM_T = TypeVar("_ITEM_T")
_SIZE_T = TypeVar("_SIZE_T")
_SCHEMA_T = TypeVar("_SCHEMA_T")
_STATE_T = TypeVar("_STATE_T")

_type_variables: WeakKeyDictionary[TypeVar, _Type] = WeakKeyDictionary()
# Release nanobind wrappers before its shutdown leak check runs.
atexit.register(_type_variables.clear)


class _StateAnnotation:
    __slots__ = ("factory",)

    def __init__(self, factory: Any):
        if not callable(factory):
            raise TypeError("State[...] requires a zero-argument callable or class")
        self.factory = factory


class State(Generic[_STATE_T]):
    """Inject one lazily created, per-runtime-node Python state object."""

    def __class_getitem__(cls, factory: Any) -> _StateAnnotation:
        return _StateAnnotation(factory)


class Output:
    """Inject a read-only, callback-scoped view of a node's current output."""


def _is_injectable_annotation(annotation: Any) -> bool:
    return (
        annotation is EvaluationClock
        or annotation is Scheduler
        or annotation is Output
        or annotation is Queue
        or isinstance(annotation, _StateAnnotation)
    )


def _annotation_arguments(arguments: Any, required: int, name: str) -> tuple[Any, Any]:
    if isinstance(arguments, tuple):
        if len(arguments) not in (required, required + 1):
            raise TypeError(f"{name}[...] accepts {required} type argument(s) and an optional guarantee")
        return arguments[0], arguments[1] if len(arguments) == required + 1 else None
    return arguments, None


def _with_guarantee(annotation: Any, guarantee: Any) -> Any:
    if guarantee is None:
        return annotation
    if not isinstance(guarantee, _TimeSeriesGuarantee):
        raise TypeError("time-series guarantees must be bg.ordinary, bg.deduplicated, bg.final, or bg.const_")
    return Annotated[annotation, guarantee]


class TS(TimeSeries, Generic[_SCALAR_T]):
    """A time-series type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        scalar_type, guarantee = _annotation_arguments(arguments, 1, "TS")
        return _with_guarantee(_time_series_type(_native_type(scalar_type)), guarantee)


class _ScalarAnnotation:
    __slots__ = ("type",)

    def __init__(self, scalar_type: Any):
        self.type = scalar_type


class _ScalarOnly:
    pass


class Scalar(Generic[_SCALAR_T]):
    """A scalar-only input annotation for nodes and graphs."""

    def __class_getitem__(cls, scalar_type: type) -> Any:
        return Annotated[scalar_type, _ScalarOnly]


class REF(TimeSeries, Generic[_TIME_SERIES_T]):
    """A time-series reference type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        target_type, guarantee = _annotation_arguments(arguments, 1, "REF")
        return _with_guarantee(_reference_type(_native_type(target_type)), guarantee)


class TSS(TimeSeries, Generic[_SCALAR_T]):
    """A time-series set type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        item_type, guarantee = _annotation_arguments(arguments, 1, "TSS")
        return _with_guarantee(_time_series_set_type(_native_type(item_type)), guarantee)


class TSL(TimeSeries, Generic[_TIME_SERIES_T, _SIZE_T]):
    """A fixed time-series list type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        if not isinstance(arguments, tuple) or len(arguments) not in (2, 3):
            raise TypeError("TSL[...] requires an item type and size, with an optional guarantee")
        item_type, size, *guarantee = arguments
        if size is SIZE:
            if guarantee:
                raise ValueError("a time-series guarantee requires a concrete time-series type")
            time_series_type = _variadic_time_series_list_type(_native_type(item_type))
        else:
            time_series_type = _time_series_list_type(_native_type(item_type), size)
        return _with_guarantee(time_series_type, guarantee[0] if guarantee else None)


class TSD(TimeSeries, Generic[_SCALAR_T, _TIME_SERIES_T]):
    """A time-series dictionary type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        if not isinstance(arguments, tuple) or len(arguments) not in (2, 3):
            raise TypeError("TSD[...] requires key and item types, with an optional guarantee")
        key_type, item_type, *guarantee = arguments
        time_series_type = _time_series_dict_type(_native_type(key_type), _native_type(item_type))
        return _with_guarantee(time_series_type, guarantee[0] if guarantee else None)


def _schema_constructor(field_names: tuple[str, ...], optional_fields: dict[str, bool]):
    def initialize(self: Any, *args: Any, **kwargs: Any) -> None:
        if len(args) > len(field_names):
            raise TypeError(f"expected at most {len(field_names)} positional values, received {len(args)}")

        values = dict(zip(field_names, args))
        for name, value in kwargs.items():
            if name not in optional_fields:
                raise TypeError(f"unexpected schema field {name!r}")
            if name in values:
                raise TypeError(f"schema field {name!r} received more than once")
            values[name] = value

        for name in field_names:
            if name in values:
                continue
            if optional_fields[name]:
                values[name] = None
                continue
            raise TypeError(f"missing required schema field {name!r}")

        for name in field_names:
            setattr(self, name, values[name])

    return initialize


def schema(cls: type) -> type:
    """Declare one scalar and time-series schema from an annotated class."""
    schema_bases = [base for base in cls.__bases__ if base is not object]
    if len(schema_bases) > 1:
        raise TypeError("schemas support one schema base")
    base = schema_bases[0] if schema_bases else None
    if base is not None and "_native_schema" not in base.__dict__:
        raise TypeError("a schema subclass requires a @schema base")

    direct_annotations = cls.__dict__.get("__annotations__", {})
    try:
        resolved_annotations = get_type_hints(cls, include_extras=True, localns={cls.__name__: cls})
    except NameError as error:
        raise TypeError("schema field annotations must resolve when @schema runs") from error

    fields = []
    for name, annotation in direct_annotations.items():
        resolved = resolved_annotations.get(name, annotation)
        union_arguments = get_args(resolved)
        if len(union_arguments) == 2 and type(None) in union_arguments:
            recursive_type = next(value for value in union_arguments if value is not type(None))
            if recursive_type is cls:
                fields.append((name, None))
                continue
        if resolved is cls:
            fields.append((name, Ellipsis))
            continue

        field_type = _native_type(resolved)
        if isinstance(field_type, _ScalarAnnotation):
            raise TypeError("schema fields cannot use Scalar[...] annotations")
        fields.append((name, field_type))

    name = f"{cls.__module__}.{cls.__qualname__}"
    base_scalar = base._native_schema if base is not None else None
    base_time_series = base._native_time_series_schema if base is not None else None
    cls._native_schema, cls._native_time_series_schema = _make_schema(
        name, base_scalar, base_time_series, fields
    )

    field_names = list(base._schema_field_names) if base is not None else []
    optional_fields = dict(base._schema_optional_fields) if base is not None else {}
    for field_name, field_type in fields:
        if field_name not in field_names:
            field_names.append(field_name)
        optional_fields[field_name] = (
            field_type is None or field_type.accepts_absent_scalar
            if field_type is not Ellipsis
            else False
        )

    cls._schema_field_names = tuple(field_names)
    cls._schema_optional_fields = optional_fields
    cls._bundle_compatible = (base is None or base._bundle_compatible) and all(
        field_type is None or field_type is Ellipsis or not field_type.is_time_series
        for _, field_type in fields
    )
    if cls._bundle_compatible:
        cls._native_bundle_type = _register_python_schema(cls, cls._native_schema)

    if "__init__" not in cls.__dict__:
        cls.__init__ = _schema_constructor(cls._schema_field_names, optional_fields)
    return cls


class Bundle(Generic[_SCHEMA_T]):
    """A hierarchical scalar type declared by ``@schema``."""

    def __class_getitem__(cls, schema_type: type) -> Any:
        if not isinstance(schema_type, type) or "_native_schema" not in schema_type.__dict__:
            raise TypeError("Bundle[...] requires a class declared with @schema")
        return _register_python_schema(schema_type, schema_type._native_schema)


class TSB(TimeSeries, Generic[_SCHEMA_T]):
    """A time-series bundle type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        schema, guarantee = _annotation_arguments(arguments, 1, "TSB")
        if schema is TS_SCHEMA:
            if guarantee is not None:
                raise ValueError("a time-series guarantee requires a concrete time-series type")
            return _with_guarantee(_variadic_time_series_bundle_type(), guarantee)
        if not isinstance(schema, type) or "_native_time_series_schema" not in schema.__dict__:
            raise TypeError("TSB[...] requires a class declared with @schema")
        return _with_guarantee(_time_series_bundle_type(schema._native_time_series_schema), guarantee)


class _TypeVariable:
    __slots__ = ("name",)

    def __init__(self, name: str):
        self.name = name

    def __repr__(self) -> str:
        return self.name


SIZE = _TypeVariable("SIZE")
TS_SCHEMA = _TypeVariable("TS_SCHEMA")


class _PassThrough:
    __slots__ = ("value",)

    def __init__(self, value: Any):
        self.value = value


def pass_through(value: Any) -> Any:
    """Keep a time series as one shared input to every map or mesh branch."""
    return _PassThrough(value)


def opt(point: Any) -> Any:
    """Require an optional time-series input at the receiving parameter."""
    if not isinstance(point, _WiringPoint):
        raise TypeError("opt expects a time-series wiring point")
    return point._as_optional()


def _map_arguments(args: tuple[Any, ...], kwargs: dict[str, Any]) -> tuple[Any, Any]:
    positional = tuple(
        (argument.value, True) if isinstance(argument, _PassThrough) else (argument, False)
        for argument in args
    )
    named = tuple(
        (name, argument.value, True) if isinstance(argument, _PassThrough) else (name, argument, False)
        for name, argument in kwargs.items()
    )
    return positional, named


def map(function: GraphDefinition, *args: Any, **kwargs: Any) -> Any:
    """Create one inner graph for every selected key."""
    if not isinstance(function, GraphDefinition):
        raise TypeError("map expects a decorated @graph definition")
    positional, named = _map_arguments(args, kwargs)
    return _map(function._native_definition, positional, named)


class _MeshSelector:
    __slots__ = ("name",)

    def __init__(self, name: str = ""):
        self.name = name

    def __getitem__(self, key: Any) -> Any:
        return _mesh_dependency(key, self.name)


def mesh(function: GraphDefinition | str | None = None, *args: Any, name: str | None = None, **kwargs: Any) -> Any:
    """Create dependent inner graphs or select a mesh branch by key."""
    if function is None:
        if args or kwargs or name is not None:
            raise TypeError("mesh() does not accept arguments when selecting the current mesh")
        return _MeshSelector()
    if isinstance(function, str):
        if args or kwargs or name is not None:
            raise TypeError("mesh(name) does not accept other arguments")
        return _MeshSelector(function)
    if not isinstance(function, GraphDefinition):
        raise TypeError("mesh expects a decorated @graph definition")

    positional, named = _map_arguments(args, kwargs)
    return _mesh(function._native_definition, positional, named, name)


class _Default:
    def __repr__(self) -> str:
        return "DEFAULT"


DEFAULT = _Default()


def switch_(key: Any, cases: dict[Any, GraphDefinition], *args: Any, **kwargs: Any) -> Any:
    """Run the graph selected by the current key value."""
    if not isinstance(cases, dict):
        raise TypeError("switch_ cases must be a dictionary")

    native_cases = []
    for case_key, function in cases.items():
        if not isinstance(function, GraphDefinition):
            raise TypeError("every switch_ case must contain a decorated @graph definition")
        if case_key is DEFAULT:
            native_cases.append((True, (), function._native_definition))
        elif isinstance(case_key, tuple):
            native_cases.append((False, case_key, function._native_definition))
        else:
            native_cases.append((False, (case_key,), function._native_definition))

    named = tuple(kwargs.items())
    return _switch(key, tuple(native_cases), args, named)


class NodeDefinition:
    def __init__(
        self,
        function: Any,
        native_definition: Any,
        fixed_parameters: tuple[str, ...],
        callback: Any,
        injected_parameters: tuple[str, ...],
        lifecycle_registrar: Any,
        callback_references: dict[str, Any],
    ):
        self.function = function
        self._callback = callback
        self._native_definition = native_definition
        self._fixed_parameters = fixed_parameters
        self._lifecycle_registrar = lifecycle_registrar
        self._callback_references = callback_references
        update_wrapper(self, function)
        signature = inspect.signature(function)
        if injected_parameters:
            signature = signature.replace(
                parameters=tuple(
                    parameter for parameter in signature.parameters.values()
                    if parameter.name not in injected_parameters
                )
            )
        self.signature = signature
        self.__signature__ = signature

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        fixed, positional, named = _bind_definition_arguments(
            self.signature, self._fixed_parameters, args, kwargs
        )
        return _wire_node(self._native_definition, fixed, positional, named)

    def start(self, function: Any) -> Any:
        """Register the node start callback using selected definition arguments."""
        self._lifecycle_registrar(self, "start", function)
        return function

    def stop(self, function: Any) -> Any:
        """Register the node stop callback using selected definition arguments."""
        self._lifecycle_registrar(self, "stop", function)
        return function

    def on_invalidation(self, function: Any) -> Any:
        """Register the node invalidation callback using selected definition arguments."""
        self._lifecycle_registrar(self, "invalidation", function)
        return function

class GraphDefinition:
    def __init__(
        self,
        function: Any,
        native_definition: Any,
        fixed_parameters: tuple[str, ...],
        callback: Any,
    ):
        self.function = function
        self._callback = callback
        self._native_definition = native_definition
        self._fixed_parameters = fixed_parameters
        update_wrapper(self, function)
        self.signature = inspect.signature(function)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        fixed, positional, named = _bind_definition_arguments(
            self.signature, self._fixed_parameters, args, kwargs
        )
        return _wire_graph(self._native_definition, fixed, positional, named)

    def __getitem__(self, input_types: Any) -> GraphDefinition:
        if not isinstance(input_types, tuple):
            input_types = (input_types,)
        native = _specialize_graph(self._native_definition, input_types)
        return GraphDefinition(self.function, native, self._fixed_parameters, self._callback)


class _ServiceDefinition:
    def __init__(self, function: Any, native_definition: Any):
        self.function = function
        self._native_definition = native_definition
        update_wrapper(self, function)
        self.signature = inspect.signature(function)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        bound = self.signature.bind(*args, **kwargs)
        bound.apply_defaults()
        arguments = tuple(bound.arguments[parameter] for parameter in self.signature.parameters)
        return _wire_service(self._native_definition, arguments)


class ServiceCatalogue:
    """Register Python graph implementations for service declarations."""

    def __init__(self):
        self._native_catalogue = _make_service_catalogue()

    def register_impl(
        self, service: _ServiceDefinition, implementation: GraphDefinition, *args: Any, **kwargs: Any
    ) -> None:
        if not isinstance(service, _ServiceDefinition):
            raise TypeError("register_impl expects a Python service declaration")
        if not isinstance(implementation, GraphDefinition):
            raise TypeError("register_impl expects a decorated @graph implementation")
        _register_service_impl(self._native_catalogue, service, implementation, args, kwargs)


def _bind_definition_arguments(
    signature: inspect.Signature,
    fixed_parameters: tuple[str, ...],
    args: tuple[Any, ...],
    kwargs: dict[str, Any],
) -> tuple[tuple[Any, ...], tuple[Any, ...], dict[str, Any]]:
    positional_pack = next(
        (
            parameter
            for parameter in signature.parameters.values()
            if parameter.kind is parameter.VAR_POSITIONAL
        ),
        None,
    )
    direct_positional = {}
    if positional_pack is not None and positional_pack.name in kwargs:
        kwargs = dict(kwargs)
        direct_positional[positional_pack.name] = kwargs.pop(positional_pack.name)

    bound = signature.bind(*args, **kwargs)
    bound.apply_defaults()

    fixed = tuple(bound.arguments[name] for name in fixed_parameters)
    positional = next(
        (
            tuple(bound.arguments[parameter.name])
            for parameter in signature.parameters.values()
            if parameter.kind is parameter.VAR_POSITIONAL
        ),
        (),
    )
    named = next(
        (
            dict(bound.arguments[parameter.name])
            for parameter in signature.parameters.values()
            if parameter.kind is parameter.VAR_KEYWORD
        ),
        {},
    )
    named = direct_positional | named

    return fixed, positional, named


def _validate_runner_options(
    engine: EvaluationEngine,
    start_time: datetime | None,
    stop_time: datetime | None,
    run_time: timedelta | None,
) -> None:
    if not isinstance(engine, EvaluationEngine):
        raise TypeError("engine must be a bg.EvaluationEngine")
    if start_time is not None and not isinstance(start_time, datetime):
        raise TypeError("start_time must be a datetime or None")
    if start_time is not None and start_time.utcoffset() is not None:
        raise ValueError("start_time must be a timezone-naive datetime")
    if stop_time is not None and not isinstance(stop_time, datetime):
        raise TypeError("stop_time must be a datetime or None")
    if stop_time is not None and stop_time.utcoffset() is not None:
        raise ValueError("stop_time must be a timezone-naive datetime")
    if run_time is not None and not isinstance(run_time, timedelta):
        raise TypeError("run_time must be a datetime.timedelta or None")
    if run_time is not None and run_time <= timedelta(0):
        raise ValueError("run_time must be positive")
    if run_time is not None and run_time > timedelta(microseconds=(1 << 63) - 1):
        raise OverflowError("run_time is outside the EvaluationTime::duration range")


def _definition_signature(
    function: Any,
    *,
    scalar_or_time_series_inputs: bool = False,
    promote_plain_graph_inputs: bool = False,
    allow_injectables: bool = False,
    allow_queue: bool = False,
) -> tuple[list[tuple[str, Any, str]], tuple[str, ...], Any, tuple[str, ...], Any]:
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    fixed = []
    variadic = []
    injected_parameters = []
    state_factory = None

    for parameter in signature.parameters.values():
        annotation = annotations.get(parameter.name, parameter.annotation)
        if annotation is inspect.Parameter.empty:
            raise TypeError(f"Python definition parameter {parameter.name!r} requires a type annotation")

        if isinstance(annotation, _StateAnnotation):
            injected_parameter = "state"
            if state_factory is not None:
                raise TypeError("a Python node can declare only one State parameter")
            state_factory = annotation.factory
        elif annotation is EvaluationClock:
            injected_parameter = "evaluation_clock"
        elif annotation is Scheduler:
            injected_parameter = "scheduler"
        elif annotation is Output:
            injected_parameter = "output"
        elif annotation is Queue:
            if not allow_queue:
                raise TypeError("bg.Queue can only annotate a bg.queue start parameter")
            injected_parameter = "queue"
        else:
            injected_parameter = None
        if injected_parameter is not None:
            if not allow_injectables:
                raise TypeError("injectables can only annotate Python node parameters")
            if parameter.kind not in (
                parameter.POSITIONAL_ONLY,
                parameter.POSITIONAL_OR_KEYWORD,
                parameter.KEYWORD_ONLY,
            ):
                raise TypeError("Python node injectables cannot annotate variadic parameters")
            if parameter.default is not None:
                raise TypeError("Python node injectables must use '= None' as their default")
            injected_parameters.append(parameter.name)
            continue

        if parameter.kind in (
            parameter.POSITIONAL_ONLY,
            parameter.POSITIONAL_OR_KEYWORD,
            parameter.KEYWORD_ONLY,
        ):
            native_type = _native_type(annotation)
            scalar_only = isinstance(native_type, _ScalarAnnotation)
            if scalar_only:
                native_type = native_type.type
                if native_type.is_time_series:
                    raise TypeError("Scalar[...] parameters must use scalar annotations, not TS[...] annotations")
            if scalar_or_time_series_inputs:
                if native_type.is_time_series:
                    raise TypeError("@node parameters must use scalar annotations, not TS[...] annotations")
                if not scalar_only:
                    native_type = _node_input_type(native_type)
            elif promote_plain_graph_inputs and not scalar_only and not native_type.is_time_series:
                native_type = _node_input_type(native_type)
            fixed.append((parameter.name, native_type, ""))
        elif parameter.kind is parameter.VAR_POSITIONAL:
            if scalar_or_time_series_inputs:
                raise TypeError("@node does not currently support variadic parameters")
            variadic.append((parameter.name, _native_type(annotation), "positional"))
        elif parameter.kind is parameter.VAR_KEYWORD:
            if scalar_or_time_series_inputs:
                raise TypeError("@node does not currently support variadic parameters")
            variadic.append((parameter.name, _native_type(annotation), "named"))
        else:
            raise TypeError(f"unsupported Python parameter kind for {parameter.name!r}")

    parameters = fixed + variadic
    fixed_names = tuple(name for name, _, _ in fixed)
    needs_positional_callback = variadic or any(
        parameter.kind is parameter.KEYWORD_ONLY for parameter in signature.parameters.values()
    )
    callback = _positional_callback(function) if needs_positional_callback else function
    return (
        parameters,
        fixed_names,
        callback,
        tuple(injected_parameters),
        state_factory,
    )


def _positional_callback(function: Any) -> Any:
    code = function.__code__
    argument_count = (
        code.co_argcount
        + code.co_kwonlyargcount
        + bool(code.co_flags & inspect.CO_VARARGS)
        + bool(code.co_flags & inspect.CO_VARKEYWORDS)
    )
    code = code.replace(
        co_flags=code.co_flags & ~(inspect.CO_VARARGS | inspect.CO_VARKEYWORDS),
        co_argcount=argument_count,
        co_posonlyargcount=0,
        co_kwonlyargcount=0,
    )
    return types.FunctionType(code, function.__globals__, function.__name__, closure=function.__closure__)


def _callback_arguments(function: Any, base_function: Any) -> tuple[Any, tuple[tuple[str, int], ...]]:
    base_signature = inspect.signature(base_function)
    base_annotations = get_type_hints(base_function, include_extras=True)
    base_arguments = {}
    input_parameters = []
    for parameter in base_signature.parameters.values():
        annotation = base_annotations.get(parameter.name, parameter.annotation)
        if annotation is EvaluationClock:
            base_arguments[parameter.name] = ("evaluation_clock", 0)
        elif annotation is Scheduler:
            base_arguments[parameter.name] = ("scheduler", 0)
        elif annotation is Output:
            base_arguments[parameter.name] = ("output", 0)
        elif annotation is Queue:
            base_arguments[parameter.name] = ("queue", 0)
        elif isinstance(annotation, _StateAnnotation):
            base_arguments[parameter.name] = ("state", 0)
        else:
            input_parameters.append(parameter)

    fixed_inputs = [
        parameter for parameter in input_parameters
        if parameter.kind not in (parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD)
    ]
    variadic_inputs = [
        parameter for parameter in input_parameters
        if parameter.kind in (parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD)
    ]
    for input_index, parameter in enumerate(fixed_inputs + variadic_inputs):
        base_arguments[parameter.name] = ("input", input_index)

    if function is base_function:
        callback_function = function
        if any(
            parameter.kind in (parameter.KEYWORD_ONLY, parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD)
            for parameter in base_signature.parameters.values()
        ):
            callback_function = _positional_callback(function)
        selected_arguments = tuple(base_arguments[parameter.name] for parameter in base_signature.parameters.values())
    else:
        signature = inspect.signature(function)
        selected = []
        for parameter in signature.parameters.values():
            if parameter.kind not in (
                parameter.POSITIONAL_ONLY,
                parameter.POSITIONAL_OR_KEYWORD,
                parameter.KEYWORD_ONLY,
            ):
                raise TypeError("Python node lifecycle callbacks do not support variadic parameters")
            if parameter.name not in base_arguments:
                raise TypeError(
                    f"Python node lifecycle parameter {parameter.name!r} is not a parameter of {base_function.__name__!r}"
                )
            selected.append(base_arguments[parameter.name])
        callback_function = function
        if any(parameter.kind is parameter.KEYWORD_ONLY for parameter in signature.parameters.values()):
            callback_function = _positional_callback(function)
        selected_arguments = tuple(selected)

    return callback_function, selected_arguments


def _define_node(
    function: Any, *, schedule_on_start: bool = False, scalar_or_time_series_inputs: bool = False,
    source: bool = False, generator: bool = False, queue_mode: QueueMode | None = None,
) -> NodeDefinition:
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    (
        parameters,
        fixed_parameters,
        callback,
        injected_parameters,
        state_factory,
    ) = _definition_signature(
        function, scalar_or_time_series_inputs=scalar_or_time_series_inputs, allow_injectables=True,
        allow_queue=queue_mode is not None,
    )

    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty or output is None or output is type(None):
        output = None
    else:
        output_type = _native_type(output)
        if isinstance(output_type, _ScalarAnnotation):
            raise TypeError("Scalar[...] can only annotate a Python node or graph input")
        if scalar_or_time_series_inputs:
            if output_type.is_time_series:
                raise TypeError("@node return annotations must be scalar annotations, not TS[...] annotations")
            output = _time_series_type(output_type)
        else:
            output = output_type

    queue_enabled = queue_mode is not None
    if queue_enabled:
        if sum(annotations.get(name) is Queue for name in injected_parameters) != 1:
            raise TypeError("a bg.queue start callback must declare exactly one Queue parameter")
        if any(annotations.get(name) is Output or annotations.get(name) is Scheduler for name in injected_parameters):
            raise TypeError("bg.queue callbacks cannot use Output or Scheduler")
        if any(native_type.is_time_series for _, native_type, _ in parameters):
            raise TypeError("bg.queue start callbacks accept scalar wiring parameters only")
        if output is None or not output.is_time_series:
            raise TypeError("bg.queue requires a TS[...] return annotation")

    if output is None and any(annotations.get(name) is Output for name in injected_parameters):
        raise TypeError("bg.Output requires a Python node with a time-series output")

    callbacks: dict[str, Any] = {}
    callback_references: dict[str, Any] = {}
    callbacks["start" if queue_enabled else "evaluation"] = callback

    def make_native_definition(callbacks_to_bind: dict[str, Any]) -> tuple[Any, dict[str, Any]]:
        callback_definitions = {}
        retained_callbacks = {}
        for name in ("evaluation", "start", "stop", "invalidation"):
            callback_function = callbacks_to_bind.get(name)
            if callback_function is None:
                callback_definitions[name] = (None, ())
            else:
                adapted, arguments = _callback_arguments(callback_function, function)
                callback_definitions[name] = (adapted, arguments)
                retained_callbacks[name] = adapted

        native_definition = _make_node(
            function.__name__, parameters, output,
            *(callback_definitions[name][0] for name in ("evaluation", "start", "stop", "invalidation")),
            schedule_on_start, scalar_or_time_series_inputs,
            *(list(callback_definitions[name][1]) for name in ("evaluation", "start", "stop", "invalidation")),
            state_factory, generator, queue_mode if queue_mode is not None else QueueMode.standard,
        )
        return native_definition, retained_callbacks

    def register_lifecycle(definition: NodeDefinition, name: str, hook: Any) -> None:
        if not callable(hook):
            raise TypeError(f"Python node {name} callback must be callable")
        if name == "start" and generator:
            raise TypeError("Python generators cannot register a separate start callback")
        if name == "invalidation" and (source or generator):
            raise TypeError("Python sources cannot register invalidation callbacks without time-series inputs")
        if name in callbacks:
            raise TypeError(f"Python node {name} callback is already defined")
        candidate_callbacks = callbacks.copy()
        candidate_callbacks[name] = hook
        native_definition, retained_callbacks = make_native_definition(candidate_callbacks)
        callbacks[name] = hook
        callback_references.update(retained_callbacks)
        definition._native_definition = native_definition

    native_definition, retained_callbacks = make_native_definition(callbacks)
    callback_references.update(retained_callbacks)
    definition = NodeDefinition(
        function, native_definition, fixed_parameters, callback, injected_parameters, register_lifecycle,
        callback_references,
    )
    return definition


def _define_service(function: Any, kind: str) -> _ServiceDefinition:
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    parameters = []
    for parameter in signature.parameters.values():
        if parameter.kind in (parameter.VAR_POSITIONAL, parameter.VAR_KEYWORD):
            raise TypeError("Python services do not support variadic parameters")
        annotation = annotations.get(parameter.name, parameter.annotation)
        if annotation is inspect.Parameter.empty:
            raise TypeError(f"Python service parameter {parameter.name!r} requires a type annotation")
        native_type = _native_type(annotation)
        if isinstance(native_type, _ScalarAnnotation):
            native_type = native_type.type
        parameters.append((parameter.name, native_type, ""))

    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty or output is None or output is type(None):
        output = None
    else:
        output = _native_type(output)
        if isinstance(output, _ScalarAnnotation) or not output.is_time_series:
            raise TypeError("Python service outputs must use an explicit TS[...] annotation")

    native_definition = _make_service(function.__name__, kind, parameters, output)
    return _ServiceDefinition(function, native_definition)


def _is_time_series_type_variable(annotation: TypeVar) -> bool:
    bound = annotation.__bound__
    return isinstance(bound, type) and issubclass(bound, TimeSeries)


def _native_type(annotation: Any) -> Any:
    if isinstance(annotation, _Type):
        return annotation
    if annotation is TimeSeries:
        raise TypeError("bg.TimeSeries is only valid as a TypeVar bound")
    if isinstance(annotation, TypeVar):
        existing = _type_variables.get(annotation)
        if existing is not None:
            return existing

        if _is_time_series_type_variable(annotation):
            descriptor = _time_series_variable_type(annotation.__name__)
            _type_variables[annotation] = descriptor
            return descriptor

        alternatives = annotation.__constraints__
        if not alternatives and annotation.__bound__ is not None:
            alternatives = (annotation.__bound__,)
        if any(
            candidate is TimeSeries or (isinstance(candidate, type) and issubclass(candidate, TimeSeries))
            for candidate in alternatives
        ):
            raise TypeError(f"TypeVar {annotation.__name__!r} must use bound=bg.TimeSeries")
        try:
            scalar_alternatives = [_native_type(candidate) for candidate in alternatives]
        except TypeError as error:
            raise TypeError(f"TypeVar {annotation.__name__!r} must be bounded by scalar types") from error
        if any(candidate.is_time_series for candidate in scalar_alternatives):
            raise TypeError(f"TypeVar {annotation.__name__!r} must be bounded by scalar types")
        descriptor = _scalar_variable_type(annotation.__name__, scalar_alternatives)
        _type_variables[annotation] = descriptor
        return descriptor
    if get_origin(annotation) is Annotated:
        value_type, *metadata = get_args(annotation)
        native_type = _native_type(value_type)
        if _ScalarOnly in metadata:
            return _ScalarAnnotation(native_type)
        for item in metadata:
            if isinstance(item, _TimeSeriesGuarantee):
                native_type = _with_time_series_guarantee(native_type, item)
            elif item is valid_items:
                native_type = _with_valid_items(native_type)
        return native_type
    origin = get_origin(annotation)
    if annotation is list or origin is list:
        raise TypeError("list annotations are mutable; use tuple[T, ...] for an engine list scalar")
    if annotation is tuple or annotation is Tuple or origin is tuple:
        arguments = get_args(annotation)
        if len(arguments) == 2 and arguments[1] is Ellipsis:
            item_type = _native_type(arguments[0])
            try:
                return _list_scalar_type(item_type)
            except ValueError as error:
                raise TypeError(str(error)) from error

        if annotation is tuple or annotation is Tuple:
            raise TypeError("tuple annotations must specify item types")

        item_types = tuple(_native_type(item) for item in arguments)
        try:
            return _tuple_scalar_type(item_types)
        except ValueError as error:
            raise TypeError(str(error)) from error
    if isinstance(annotation, type):
        if "_native_schema" in annotation.__dict__:
            if not annotation._bundle_compatible:
                raise TypeError("a scalar Bundle schema must contain only scalar fields")
            return annotation._native_bundle_type
        return _scalar_type(annotation)
    if origin is dict:
        key_type, value_type = get_args(annotation)
        return _dict_scalar_type(_native_type(key_type), _native_type(value_type))
    if get_origin(annotation) is list:
        (item_type,) = get_args(annotation)
        return _list_scalar_type(_native_type(item_type))
    union_arguments = get_args(annotation)
    if len(union_arguments) == 2 and type(None) in union_arguments:
        value_type = next(value for value in union_arguments if value is not type(None))
        native_type = _native_type(value_type)
        if isinstance(native_type, _ScalarAnnotation):
            return _ScalarAnnotation(_optional_type(native_type.type))
        return _optional_type(native_type)
    raise TypeError(f"unsupported Python node annotation: {annotation!r}")


def compute_node(function: Any) -> NodeDefinition:
    """Declare a Python node that runs when one of its inputs changes."""
    return _define_node(function)


def reference_service(function: Any) -> _ServiceDefinition:
    """Declare a source-like service with scalar lookup parameters."""
    return _define_service(function, "reference")


def service(function: Any) -> _ServiceDefinition:
    """Declare a request/reply service with time-series inputs."""
    return _define_service(function, "request_reply")


def subscription_service(function: Any) -> _ServiceDefinition:
    """Declare a service that requests values for final time-series keys."""
    return _define_service(function, "subscription")


def node(function: Any) -> NodeDefinition:
    """Declare a Python node whose scalar annotations accept scalar or TS[T] inputs."""
    return _define_node(function, schedule_on_start=True, scalar_or_time_series_inputs=True)


def source_node(function: Any) -> NodeDefinition:
    """Declare a source that evaluates at graph start and can schedule later evaluations."""
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    for parameter in signature.parameters.values():
        annotation = annotations.get(parameter.name, parameter.annotation)
        if _is_injectable_annotation(annotation):
            continue
        input_type = _native_type(annotation)
        if isinstance(input_type, _ScalarAnnotation):
            input_type = input_type.type
        if input_type.is_time_series:
            raise TypeError("Python source node parameters must be scalar wiring values")
    output = annotations.get("return", signature.return_annotation)
    output_type = _native_type(output) if output is not inspect.Signature.empty else None
    if output_type is None or not output_type.is_time_series:
        raise TypeError("Python source nodes must return a time-series value")

    return _define_node(function, schedule_on_start=True, source=True)


def queue(function: Any = None, *, mode: QueueMode = QueueMode.standard) -> Any:
    """Declare a push queue node whose start callback writes its output ticks."""
    if not isinstance(mode, QueueMode):
        raise TypeError("mode must be a bg.QueueMode")

    def decorate(callback: Any) -> NodeDefinition:
        if not callable(callback):
            raise TypeError("@queue requires a callable start callback")
        return _define_node(callback, source=True, queue_mode=mode)

    return decorate if function is None else decorate(function)


def generator(function: Any) -> NodeDefinition:
    """Declare a pull source that yields ``(datetime | timedelta, delta)`` pairs."""
    if not inspect.isgeneratorfunction(function):
        raise TypeError("@generator requires a Python generator function")

    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    for parameter in signature.parameters.values():
        annotation = annotations.get(parameter.name, parameter.annotation)
        if _is_injectable_annotation(annotation):
            if annotation is Scheduler:
                raise TypeError("Python generators cannot use Scheduler; yield timed output instead")
            if annotation is Output:
                raise TypeError("Python generators cannot use bg.Output; output views are callback-scoped")
            continue
        input_type = _native_type(annotation)
        if isinstance(input_type, _ScalarAnnotation):
            input_type = input_type.type
        if input_type.is_time_series:
            raise TypeError("Python generator parameters must be scalar wiring values")

    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty:
        raise TypeError("Python generators must declare a TS[...] return annotation")
    output_type = _native_type(output)
    if not output_type.is_time_series:
        raise TypeError("Python generators must return a TS[...] value")
    return _define_node(function, generator=True)


def graph(function: Any) -> GraphDefinition:
    """Declare a graph template composed from Python nodes and graph functions."""
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    parameters, fixed_parameters, callback, _, _ = _definition_signature(
        function, promote_plain_graph_inputs=True
    )

    output = annotations.get("return", signature.return_annotation)
    lift_scalar_output = False
    if output is inspect.Signature.empty or output is None or output is type(None):
        output = None
    else:
        output = _native_type(output)
        if isinstance(output, _ScalarAnnotation):
            raise TypeError("Scalar[...] can only annotate a Python node or graph input")
        if not output.is_time_series:
            output = _time_series_type(output)
            lift_scalar_output = True

    native = _make_graph(function.__name__, parameters, output, callback, lift_scalar_output)
    return GraphDefinition(function, native, fixed_parameters, callback)


def run_graph(
    definition: GraphDefinition,
    *args: Any,
    catalogue: ServiceCatalogue | None = None,
    recorder: _EvaluationProfilerRecorder | None = None,
    engine: EvaluationEngine = EvaluationEngine.simulation,
    start_time: datetime | None = None,
    stop_token: StopToken | None = None,
    stop_time: datetime | None = None,
    run_time: timedelta | None = None,
    **kwargs: Any,
) -> None:
    """Wire and run a graph with the selected evaluation engine."""
    if not isinstance(definition, GraphDefinition):
        raise TypeError("run_graph expects a decorated @graph definition")
    if recorder is not None and not isinstance(recorder, _EvaluationProfilerRecorder):
        raise TypeError("recorder must be created by EvaluationProfiler.recorder()")
    if catalogue is not None and not isinstance(catalogue, ServiceCatalogue):
        raise TypeError("catalogue must be a bg.ServiceCatalogue")
    _validate_runner_options(engine, start_time, stop_time, run_time)
    if stop_token is not None and not isinstance(stop_token, StopToken):
        raise TypeError("stop_token must be a bg.StopToken or None")

    fixed, positional, named = _bind_definition_arguments(
        definition.signature, definition._fixed_parameters, args, kwargs
    )
    native_catalogue = None if catalogue is None else catalogue._native_catalogue
    _run_graph(definition._native_definition, fixed, positional, named, native_catalogue, recorder,
               engine, start_time, stop_token, stop_time, run_time)


def test_graph(definition: GraphDefinition, *inputs: Any,
               catalogue: ServiceCatalogue | None = None,
               recorder: _EvaluationProfilerRecorder | None = None,
               engine: EvaluationEngine = EvaluationEngine.simulation,
               start_time: datetime | None = None,
               stop_time: datetime | None = None,
               run_time: timedelta | None = None) -> Any:
    """Run a graph definition with one update sequence for each time-series input."""
    if not isinstance(definition, GraphDefinition):
        raise TypeError("test_graph expects a decorated @graph definition")
    if recorder is not None and not isinstance(recorder, _EvaluationProfilerRecorder):
        raise TypeError("recorder must be created by EvaluationProfiler.recorder()")
    if catalogue is not None and not isinstance(catalogue, ServiceCatalogue):
        raise TypeError("catalogue must be a bg.ServiceCatalogue")
    _validate_runner_options(engine, start_time, stop_time, run_time)
    if engine is EvaluationEngine.realtime:
        raise ValueError("test_graph only supports the simulation engine")
    native_catalogue = None if catalogue is None else catalogue._native_catalogue
    return _test_graph(definition._native_definition, inputs, native_catalogue, recorder, engine, start_time,
                       stop_time, run_time)


operators.map = map
operators.mesh = mesh
operators.switch_ = switch_


__all__ = [
    "EvaluationEngine",
    "EvaluationClock",
    "EvaluationProfiler",
    "Queue",
    "QueueMode",
    "StopSource",
    "StopToken",
    "Bundle",
    "Scheduler",
    "ScheduleHandle",
    "const_",
    "deduplicated",
    "final",
    "GraphDefinition",
    "DEFAULT",
    "INVALID",
    "NodeDefinition",
    "ordinary",
    "Output",
    "REF",
    "REMOVE",
    "Removed",
    "Scalar",
    "State",
    "ServiceCatalogue",
    "SetDelta",
    "SIZE",
    "Symbol",
    "TS",
    "TSB",
    "TS_SCHEMA",
    "TSD",
    "TSL",
    "TSS",
    "TimeSeries",
    "compute_node",
    "generator",
    "graph",
    "map",
    "mesh",
    "node",
    "opt",
    "operators",
    "pass_through",
    "queue",
    "run_graph",
    "schema",
    "reference_service",
    "service",
    "subscription_service",
    "source_node",
    "symbol",
    "switch_",
    "version",
    "test_graph",
]

"""Python graph authoring for the bg engine."""

from __future__ import annotations

import inspect
import types
from functools import update_wrapper
from typing import Annotated, Any, get_args, get_origin, get_type_hints

from . import operators
from ._bg import (
    Dict,
    EvaluationProfiler,
    EvaluationProfilerRecorder as _EvaluationProfilerRecorder,
    _TimeSeriesGuarantee,
    WiringPoint as _WiringPoint,
    _Type,
    _make_graph,
    _make_node,
    _make_time_series_schema,
    _dict_scalar_type,
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
    _switch,
    _wire_graph,
    _wire_node,
    _with_time_series_guarantee,
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


_register_delta_types(SetDelta, Removed, REMOVE)


ordinary = _TimeSeriesGuarantee.ordinary
deduplicated = _TimeSeriesGuarantee.deduplicated
final = _TimeSeriesGuarantee.final
const_ = _TimeSeriesGuarantee.const_


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


class TS:
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


class Scalar:
    """A scalar-only input annotation for nodes and graphs."""

    def __class_getitem__(cls, scalar_type: type) -> Any:
        return Annotated[scalar_type, _ScalarOnly]


class REF:
    """A time-series reference type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        target_type, guarantee = _annotation_arguments(arguments, 1, "REF")
        return _with_guarantee(_reference_type(_native_type(target_type)), guarantee)


class TSS:
    """A time-series set type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        item_type, guarantee = _annotation_arguments(arguments, 1, "TSS")
        return _with_guarantee(_time_series_set_type(_native_type(item_type)), guarantee)


class TSL:
    """A fixed time-series list type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        if not isinstance(arguments, tuple) or len(arguments) not in (2, 3):
            raise TypeError("TSL[...] requires an item type and size, with an optional guarantee")
        item_type, size, *guarantee = arguments
        if size is SIZE:
            time_series_type = _variadic_time_series_list_type(_native_type(item_type))
        else:
            time_series_type = _time_series_list_type(_native_type(item_type), size)
        return _with_guarantee(time_series_type, guarantee[0] if guarantee else None)


class TSD:
    """A time-series dictionary type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        if not isinstance(arguments, tuple) or len(arguments) not in (2, 3):
            raise TypeError("TSD[...] requires key and item types, with an optional guarantee")
        key_type, item_type, *guarantee = arguments
        time_series_type = _time_series_dict_type(_native_type(key_type), _native_type(item_type))
        return _with_guarantee(time_series_type, guarantee[0] if guarantee else None)


class TimeSeriesSchema:
    """A named schema for a time-series bundle."""

    _native_schema: Any

    def __init_subclass__(cls) -> None:
        super().__init_subclass__()
        direct_annotations = cls.__dict__.get("__annotations__", {})
        resolved_annotations = get_type_hints(cls, include_extras=True)
        fields = [
            (name, _native_type(resolved_annotations.get(name, annotation)))
            for name, annotation in direct_annotations.items()
            if name != "_native_schema"
        ]
        schema_bases = [
            candidate
            for candidate in cls.__bases__
            if issubclass(candidate, TimeSeriesSchema) and hasattr(candidate, "_native_schema")
        ]
        if len(schema_bases) > 1:
            raise TypeError("time-series schemas support one schema base")
        base = schema_bases[0]._native_schema if schema_bases else None
        qualified_name = f"{cls.__module__}.{cls.__qualname__}"
        cls._native_schema = _make_time_series_schema(qualified_name, cls, base, fields)


class TSB:
    """A time-series bundle type annotation."""

    def __class_getitem__(cls, arguments: Any) -> Any:
        schema, guarantee = _annotation_arguments(arguments, 1, "TSB")
        if schema is TS_SCHEMA:
            return _with_guarantee(_variadic_time_series_bundle_type(), guarantee)
        if not isinstance(schema, type) or not issubclass(schema, TimeSeriesSchema):
            raise TypeError("TSB[...] requires a TimeSeriesSchema")
        return _with_guarantee(_time_series_bundle_type(schema._native_schema), guarantee)


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
        return _wire_node(self._native_definition, fixed, positional, named)


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


def _definition_signature(
    function: Any, *, scalar_or_time_series_inputs: bool = False, promote_plain_graph_inputs: bool = False
) -> tuple[list[tuple[str, Any, str]], tuple[str, ...], Any]:
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    fixed = []
    variadic = []

    for parameter in signature.parameters.values():
        annotation = annotations.get(parameter.name, parameter.annotation)
        if annotation is inspect.Parameter.empty:
            raise TypeError(f"Python definition parameter {parameter.name!r} requires a type annotation")

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
    return parameters, fixed_names, callback


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


def _define_node(
    function: Any, *, schedule_on_start: bool = False, scalar_or_time_series_inputs: bool = False
) -> NodeDefinition:
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    parameters, fixed_parameters, callback = _definition_signature(
        function, scalar_or_time_series_inputs=scalar_or_time_series_inputs
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

    native_definition = _make_node(
        function.__name__, parameters, output, callback, schedule_on_start, scalar_or_time_series_inputs
    )
    return NodeDefinition(function, native_definition, fixed_parameters, callback)


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


def _native_type(annotation: Any) -> Any:
    if isinstance(annotation, _Type):
        return annotation
    if get_origin(annotation) is Annotated:
        value_type, *metadata = get_args(annotation)
        native_type = _native_type(value_type)
        if _ScalarOnly in metadata:
            return _ScalarAnnotation(native_type)
        for item in metadata:
            if isinstance(item, _TimeSeriesGuarantee):
                native_type = _with_time_series_guarantee(native_type, item)
        return native_type
    if isinstance(annotation, type):
        return _scalar_type(annotation)
    if getattr(annotation, "__origin__", None) is dict:
        key_type, value_type = get_args(annotation)
        return _dict_scalar_type(_native_type(key_type), _native_type(value_type))
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
    """Declare a no-input node that runs in the first evaluation cycle."""
    signature = inspect.signature(function)
    if signature.parameters:
        raise TypeError("Python source nodes cannot have parameters")

    annotations = get_type_hints(function, include_extras=True)
    output = annotations.get("return", signature.return_annotation)
    output_type = _native_type(output) if output is not inspect.Signature.empty else None
    if output_type is None or not output_type.is_time_series:
        raise TypeError("Python source nodes must return a time-series value")

    return _define_node(function, schedule_on_start=True)


def graph(function: Any) -> GraphDefinition:
    """Declare a graph template composed from Python nodes and graph functions."""
    signature = inspect.signature(function)
    annotations = get_type_hints(function, include_extras=True)
    parameters, fixed_parameters, callback = _definition_signature(function, promote_plain_graph_inputs=True)

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
    **kwargs: Any,
) -> None:
    """Wire and run a graph to completion, using scalar arguments as wiring-time constants."""
    if not isinstance(definition, GraphDefinition):
        raise TypeError("run_graph expects a decorated @graph definition")
    if recorder is not None and not isinstance(recorder, _EvaluationProfilerRecorder):
        raise TypeError("recorder must be created by EvaluationProfiler.recorder()")
    if catalogue is not None and not isinstance(catalogue, ServiceCatalogue):
        raise TypeError("catalogue must be a bg.ServiceCatalogue")

    fixed, positional, named = _bind_definition_arguments(
        definition.signature, definition._fixed_parameters, args, kwargs
    )
    native_catalogue = None if catalogue is None else catalogue._native_catalogue
    _run_graph(definition._native_definition, fixed, positional, named, native_catalogue, recorder)


def test_graph(definition: GraphDefinition, *inputs: Any,
               catalogue: ServiceCatalogue | None = None,
               recorder: _EvaluationProfilerRecorder | None = None) -> Any:
    """Run a graph definition with one update sequence for each time-series input."""
    if not isinstance(definition, GraphDefinition):
        raise TypeError("test_graph expects a decorated @graph definition")
    if recorder is not None and not isinstance(recorder, _EvaluationProfilerRecorder):
        raise TypeError("recorder must be created by EvaluationProfiler.recorder()")
    if catalogue is not None and not isinstance(catalogue, ServiceCatalogue):
        raise TypeError("catalogue must be a bg.ServiceCatalogue")
    native_catalogue = None if catalogue is None else catalogue._native_catalogue
    return _test_graph(definition._native_definition, inputs, native_catalogue, recorder)


operators.map = map
operators.mesh = mesh
operators.switch_ = switch_


__all__ = [
    "EvaluationProfiler",
    "const_",
    "deduplicated",
    "final",
    "GraphDefinition",
    "DEFAULT",
    "NodeDefinition",
    "ordinary",
    "REF",
    "REMOVE",
    "Removed",
    "Scalar",
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
    "TimeSeriesSchema",
    "compute_node",
    "graph",
    "map",
    "mesh",
    "node",
    "opt",
    "operators",
    "pass_through",
    "run_graph",
    "reference_service",
    "service",
    "subscription_service",
    "source_node",
    "symbol",
    "switch_",
    "version",
    "test_graph",
]

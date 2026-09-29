"""Python graph authoring for the bg engine."""

from __future__ import annotations

import inspect
from functools import update_wrapper
from typing import Any, get_args, get_type_hints

from . import operators
from ._bg import (
    Symbol,
    _Type,
    _make_graph,
    _make_node,
    _optional_type,
    _scalar_type,
    _test_graph,
    _time_series_type,
    _wire_graph,
    _wire_node,
    symbol,
    version,
)


class TS:
    """A time-series type annotation."""

    def __class_getitem__(cls, scalar_type: type) -> Any:
        return _time_series_type(_scalar_type(scalar_type))


class NodeDefinition:
    def __init__(self, function: Any, native_definition: Any):
        self.function = function
        self._native_definition = native_definition
        update_wrapper(self, function)
        self.signature = inspect.signature(function)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        arguments = self.signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        return _wire_node(self._native_definition, tuple(arguments.arguments.values()))


class GraphDefinition:
    def __init__(self, function: Any, native_definition: Any):
        self.function = function
        self._native_definition = native_definition
        update_wrapper(self, function)
        self.signature = inspect.signature(function)

    def __call__(self, *args: Any, **kwargs: Any) -> Any:
        arguments = self.signature.bind(*args, **kwargs)
        arguments.apply_defaults()
        return _wire_graph(self._native_definition, tuple(arguments.arguments.values()))


def _define_node(function: Any, *, schedule_on_start: bool = False) -> NodeDefinition:
    parameters = []
    signature = inspect.signature(function)
    annotations = get_type_hints(function)
    for parameter in signature.parameters.values():
        if parameter.kind not in (parameter.POSITIONAL_ONLY, parameter.POSITIONAL_OR_KEYWORD):
            raise TypeError("Python node parameters must be positional or positional-or-keyword")
        annotation = annotations.get(parameter.name, parameter.annotation)
        if annotation is inspect.Parameter.empty:
            raise TypeError(f"Python node parameter {parameter.name!r} requires a type annotation")
        parameters.append((parameter.name, _native_type(annotation)))

    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty or output is None or output is type(None):
        output = None
    else:
        output = _native_type(output)

    native_definition = _make_node(function.__name__, parameters, output, function, schedule_on_start)
    return NodeDefinition(function, native_definition)


def _native_type(annotation: Any) -> Any:
    if isinstance(annotation, _Type):
        return annotation
    if isinstance(annotation, type):
        return _scalar_type(annotation)
    union_arguments = get_args(annotation)
    if len(union_arguments) == 2 and type(None) in union_arguments:
        value_type = next(value for value in union_arguments if value is not type(None))
        return _optional_type(_native_type(value_type))
    raise TypeError(f"unsupported Python node annotation: {annotation!r}")


def compute_node(function: Any) -> NodeDefinition:
    """Declare a Python node that runs when one of its inputs changes."""
    return _define_node(function)


def source_node(function: Any) -> NodeDefinition:
    """Declare a no-input node that runs in the first evaluation cycle."""
    signature = inspect.signature(function)
    if signature.parameters:
        raise TypeError("Python source nodes cannot have parameters")

    annotations = get_type_hints(function)
    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty or not isinstance(output, _Type) or not output.is_time_series:
        raise TypeError("Python source nodes must return a TS[...] value")

    return _define_node(function, schedule_on_start=True)


def graph(function: Any) -> GraphDefinition:
    """Declare a graph template composed from Python nodes and graph functions."""
    signature = inspect.signature(function)
    annotations = get_type_hints(function)
    parameters = []
    for parameter in signature.parameters.values():
        if parameter.kind not in (parameter.POSITIONAL_ONLY, parameter.POSITIONAL_OR_KEYWORD):
            raise TypeError("Python graph parameters must be positional or positional-or-keyword")
        annotation = annotations.get(parameter.name, parameter.annotation)
        if annotation is inspect.Parameter.empty:
            raise TypeError(f"Python graph parameter {parameter.name!r} requires a type annotation")
        parameters.append((parameter.name, _native_type(annotation)))

    output = annotations.get("return", signature.return_annotation)
    if output is inspect.Signature.empty or output is None or output is type(None):
        output = None
    else:
        output = _native_type(output)

    return GraphDefinition(function, _make_graph(function.__name__, parameters, output, function))


def test_graph(definition: GraphDefinition, *inputs: Any) -> Any:
    """Run a graph definition with one update sequence for each time-series input."""
    if not isinstance(definition, GraphDefinition):
        raise TypeError("test_graph expects a decorated @graph definition")
    return _test_graph(definition._native_definition, inputs)


__all__ = [
    "GraphDefinition",
    "NodeDefinition",
    "Symbol",
    "TS",
    "compute_node",
    "graph",
    "operators",
    "source_node",
    "symbol",
    "version",
    "test_graph",
]

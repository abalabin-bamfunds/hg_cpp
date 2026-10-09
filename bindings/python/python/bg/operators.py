"""Graph operators."""

from ._bg import operators as _native

add = _native.add
const_ = _native.const_
default_ = _native.default_
getitem = _native.getitem
getattr = _native.getattr

__all__ = ["add", "const_", "default_", "getattr", "getitem", "map", "mesh", "switch_"]

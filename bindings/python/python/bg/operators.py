"""Graph operators."""

from ._bg import operators as _native

const_ = _native.const_
getitem = _native.getitem
getattr = _native.getattr

__all__ = ["const_", "getitem", "getattr"]

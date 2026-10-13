"""Graph operators."""

from ._bg import operators as _native

add = _native.add
sub = _native.sub
mul = _native.mul
div = _native.div
floordiv = _native.floordiv
mod = _native.mod
pow = _native.pow
round = _native.round
convert = _native.convert
pos = _native.pos
neg = _native.neg
abs = _native.abs
eq = _native.eq
ne = _native.ne
lt = _native.lt
le = _native.le
gt = _native.gt
ge = _native.ge
and_ = _native.and_
or_ = _native.or_
not_ = _native.not_
const_ = _native.const_
default_ = _native.default_
getitem = _native.getitem
getattr = _native.getattr

__all__ = [
    "abs",
    "add",
    "and_",
    "const_",
    "convert",
    "default_",
    "div",
    "eq",
    "floordiv",
    "ge",
    "getattr",
    "getitem",
    "gt",
    "le",
    "lt",
    "map",
    "mesh",
    "mod",
    "mul",
    "ne",
    "neg",
    "not_",
    "or_",
    "pow",
    "pos",
    "round",
    "sub",
    "switch_",
]

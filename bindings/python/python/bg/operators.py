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
valid = _native.valid
all_valid = _native.all_valid
count = _native.count
sample = _native.sample
dedup = _native.dedup
filter_ = _native.filter_
if_then_else = _native.if_then_else
lag = _native.lag
delay = _native.delay
schedule = _native.schedule
throttle = _native.throttle
sums = _native.sums
mins = _native.mins
maxs = _native.maxs
means = _native.means
len_ = _native.len_
sum_ = _native.sum_
min_ = _native.min_
max_ = _native.max_
mean = _native.mean
contains_ = _native.contains_
is_empty = _native.is_empty
bit_or = _native.bit_or
bit_and = _native.bit_and
bit_xor = _native.bit_xor
const_ = _native.const_
default_ = _native.default_
getitem = _native.getitem
getattr = _native.getattr

__all__ = [
    "abs",
    "all_valid",
    "add",
    "and_",
    "const_",
    "contains_",
    "is_empty",
    "bit_or",
    "bit_and",
    "bit_xor",
    "convert",
    "count",
    "default_",
    "delay",
    "dedup",
    "div",
    "eq",
    "floordiv",
    "filter_",
    "ge",
    "getattr",
    "getitem",
    "if_then_else",
    "gt",
    "le",
    "lag",
    "len_",
    "lt",
    "map",
    "max_",
    "mesh",
    "maxs",
    "mean",
    "means",
    "min_",
    "mins",
    "mod",
    "mul",
    "ne",
    "neg",
    "not_",
    "or_",
    "pow",
    "pos",
    "round",
    "sample",
    "schedule",
    "sub",
    "sum_",
    "switch_",
    "sums",
    "throttle",
    "valid",
]

"""Graph operators."""

from ._bg import operators as _native

add = _native.add
sub = _native.sub
mul = _native.mul
substr = _native.substr
lower = _native.lower
upper = _native.upper
casefold = _native.casefold
str_ = _native.str_
format_ = _native.format_
print_ = _native.print_
log_ = _native.log_
matches = _native.matches
replace = _native.replace
match_ = _native.match_
split = _native.split
join = _native.join
div = _native.div
floordiv = _native.floordiv
mod = _native.mod
pow = _native.pow
round = _native.round
convert = _native.convert
combine = _native.combine
collect = _native.collect
emit = _native.emit
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
merge = _native.merge
take = _native.take
drop = _native.drop
step = _native.step
if_then_else = _native.if_then_else
lag = _native.lag
diff = _native.diff
clip = _native.clip
ewma = _native.ewma
delay = _native.delay
schedule = _native.schedule
throttle = _native.throttle
resample = _native.resample
sums = _native.sums
mins = _native.mins
maxs = _native.maxs
means = _native.means
len_ = _native.len_
sum_ = _native.sum_
min_ = _native.min_
max_ = _native.max_
mean = _native.mean
all_ = _native.all_
any_ = _native.any_
contains_ = _native.contains_
is_empty = _native.is_empty
keys_where_true = _native.keys_where_true
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
    "clip",
    "const_",
    "contains_",
    "is_empty",
    "keys_where_true",
    "bit_or",
    "bit_and",
    "bit_xor",
    "convert",
    "combine",
    "collect",
    "count",
    "default_",
    "delay",
    "dedup",
    "dispatch",
    "diff",
    "drop",
    "div",
    "eq",
    "ewma",
    "floordiv",
    "filter_",
    "emit",
    "format_",
    "ge",
    "getattr",
    "getitem",
    "if_then_else",
    "gt",
    "le",
    "lower",
    "lag",
    "len_",
    "log_",
    "lt",
    "map",
    "merge",
    "match_",
    "matches",
    "max_",
    "mesh",
    "maxs",
    "mean",
    "all_",
    "any_",
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
    "print_",
    "replace",
    "pos",
    "round",
    "resample",
    "sample",
    "schedule",
    "casefold",
    "sub",
    "substr",
    "str_",
    "sum_",
    "step",
    "take",
    "switch_",
    "split",
    "join",
    "sums",
    "throttle",
    "upper",
    "valid",
]

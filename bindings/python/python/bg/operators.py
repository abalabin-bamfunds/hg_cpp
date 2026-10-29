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
gate = _native.gate
batch = _native.batch
merge = _native.merge
take = _native.take
drop = _native.drop
step = _native.step
if_then_else = _native.if_then_else
lag = _native.lag
last_modified_time = _native.last_modified_time
elapsed = _native.elapsed
diff = _native.diff
clip = _native.clip
ewma = _native.ewma
delay = _native.delay
schedule = _native.schedule
throttle = _native.throttle
debounce = _native.debounce
timeout = _native.timeout
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
keys_ = _native.keys_
keys_where_true = _native.keys_where_true
bit_or = _native.bit_or
bit_and = _native.bit_and
bit_xor = _native.bit_xor
rekey = _native.rekey
partition = _native.partition
unpartition = _native.unpartition
flip = _native.flip
flip_keys = _native.flip_keys
collapse_keys = _native.collapse_keys
uncollapse_keys = _native.uncollapse_keys
const_ = _native.const_
default_ = _native.default_
nothing = _native.nothing
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
    "keys_",
    "keys_where_true",
    "bit_or",
    "bit_and",
    "bit_xor",
    "rekey",
    "partition",
    "unpartition",
    "flip",
    "flip_keys",
    "collapse_keys",
    "uncollapse_keys",
    "convert",
    "combine",
    "collect",
    "count",
    "default_",
    "debounce",
    "delay",
    "dedup",
    "dispatch",
    "diff",
    "drop",
    "elapsed",
    "div",
    "eq",
    "ewma",
    "floordiv",
    "filter_",
    "gate",
    "batch",
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
    "last_modified_time",
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
    "nothing",
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
    "timeout",
    "upper",
    "valid",
]

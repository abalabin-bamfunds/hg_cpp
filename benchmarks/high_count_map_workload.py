"""Engine-independent workload and event checksum for the high-count map benchmark."""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import StrEnum


CHILD_COUNT = 8
UINT64_MASK = (1 << 64) - 1


@dataclass(frozen=True, slots=True)
class Profile:
    name: str
    flat_count: int
    parent_count: int
    flat_updates: int
    child_updates: int
    flat_lifecycle_count: int
    parent_lifecycle_count: int
    child_lifecycle_count: int

    @property
    def active_inner_graphs(self) -> int:
        return self.flat_count * 2 + self.parent_count * (CHILD_COUNT + 1)


PROFILES = {
    "smoke": Profile("smoke", 8, 1, 7, 1, 1, 1, 1),
    "25k": Profile("25k", 8_000, 1_000, 250, 250, 250, 25, 100),
    "100k": Profile("100k", 32_000, 4_000, 250, 250, 250, 25, 100),
    "250k": Profile("250k", 80_000, 10_000, 250, 250, 250, 25, 100),
}


class Lifecycle(StrEnum):
    NONE = "none"
    REMOVE_FLAT_AND_PARENTS = "remove_flat_and_parents"
    RESTORE_FLAT_AND_PARENTS = "restore_flat_and_parents"
    REMOVE_CHILDREN = "remove_children"
    RESTORE_CHILDREN = "restore_children"


@dataclass(frozen=True, slots=True)
class FlatMutation:
    key: int
    value: int


@dataclass(frozen=True, slots=True)
class ChildMutation:
    parent_key: int
    child_key: int
    value: int


@dataclass
class EventChecksum:
    count: int = 0
    sum: int = 0
    xor_value: int = 0

    def add(
        self,
        cycle: int,
        branch: int,
        parent_key: int,
        item_key: int,
        present: bool,
        value: int,
    ) -> None:
        event_hash = mix(cycle)
        event_hash = mix(event_hash ^ mix(branch))
        event_hash = mix(event_hash ^ mix(parent_key))
        event_hash = mix(event_hash ^ mix(item_key))
        event_hash = mix(event_hash ^ mix(1 if present else 0))
        event_hash = mix(event_hash ^ mix(value))

        self.count += 1
        self.sum = (self.sum + event_hash) & UINT64_MASK
        rotation = event_hash & 63
        rotated = ((event_hash << rotation) | (event_hash >> ((64 - rotation) & 63))) & UINT64_MASK
        self.xor_value ^= rotated


class Branch:
    FLAT = 1
    CHILD = 2


@dataclass(slots=True)
class CyclePlan:
    flat_updates: list[FlatMutation] = field(default_factory=list)
    child_updates: list[ChildMutation] = field(default_factory=list)
    flat_lifecycle_keys: list[int] = field(default_factory=list)
    parent_lifecycle_keys: list[int] = field(default_factory=list)
    child_lifecycle_keys: list[tuple[int, int]] = field(default_factory=list)
    lifecycle: Lifecycle = Lifecycle.NONE
    bias: int | None = None


@dataclass(slots=True)
class Workload:
    selected_profile: Profile
    cycles: list[CyclePlan]
    expected: EventChecksum


def mix(value: int) -> int:
    value = (value + 0x9E3779B97F4A7C15) & UINT64_MASK
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & UINT64_MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & UINT64_MASK
    return value ^ (value >> 31)


def initial_flat_value(key: int) -> int:
    return 1_000_004 + key * 17


def initial_child_value(parent_key: int, child_key: int) -> int:
    return 1_000_036 + parent_key * 131 + child_key * 19


def flat_source_value(cycle: int, key: int) -> int:
    return (cycle + 2) * 1_000_003 + key * 17 + 1


def child_source_value(cycle: int, parent_key: int, child_key: int) -> int:
    return (cycle + 2) * 1_000_033 + parent_key * 131 + child_key * 19 + 3


def stage_a_value(value: int, key: int, bias: int) -> int:
    a1 = value + bias
    a2 = a1 * 3
    a3 = a2 + key
    a4 = a3 * 5
    return a4 + 7


def stage_b_value(value: int, key: int, bias: int) -> int:
    b1 = value + bias
    b2 = b1 * 7
    b3 = b2 + key
    b4 = b3 * 3
    return b4 + 11


def child_value(value: int, parent_key: int, child_key: int, bias: int) -> int:
    p1 = parent_key + bias
    seed = p1 * 5
    c1 = value + seed
    c2 = c1 * 3
    c3 = c2 + child_key
    c4 = c3 + bias
    return c4 * 7


def _child_index(parent_key: int, child_key: int) -> int:
    return parent_key * CHILD_COUNT + child_key


def _take_keys(size: int, count: int, start: int, excluded) -> list[int]:
    keys = []
    for offset in range(size):
        if len(keys) == count:
            break
        key = (start + offset) % size
        if not excluded(key):
            keys.append(key)
    return keys


def build_workload(profile_name: str, cycle_count: int) -> Workload:
    """Build source deltas and the checksum of every expected output event."""
    try:
        selected_profile = PROFILES[profile_name]
    except KeyError as error:
        raise ValueError(f"unknown profile: {profile_name}") from error
    if cycle_count < 0:
        raise ValueError("cycle_count must be non-negative")

    result = Workload(selected_profile, [], EventChecksum())
    flat_values = [initial_flat_value(key) for key in range(selected_profile.flat_count)]
    flat_present = [True] * selected_profile.flat_count
    total_children = selected_profile.parent_count * CHILD_COUNT
    child_values = [0] * total_children
    parent_present = [True] * selected_profile.parent_count
    child_present = [True] * total_children
    for parent_key in range(selected_profile.parent_count):
        for child_key in range(CHILD_COUNT):
            child_values[_child_index(parent_key, child_key)] = initial_child_value(parent_key, child_key)

    flat_update_count = min(selected_profile.flat_updates, selected_profile.flat_count)
    child_update_count = min(selected_profile.child_updates, selected_profile.parent_count)
    flat_lifecycle_count = min(selected_profile.flat_lifecycle_count, selected_profile.flat_count)
    parent_lifecycle_count = min(selected_profile.parent_lifecycle_count, selected_profile.parent_count)
    child_lifecycle_count = min(selected_profile.child_lifecycle_count, selected_profile.parent_count)
    bias = 1

    for cycle in range(cycle_count):
        plan = CyclePlan()
        lifecycle_phase = cycle % 100

        if lifecycle_phase == 20:
            plan.lifecycle = Lifecycle.REMOVE_FLAT_AND_PARENTS
            plan.flat_lifecycle_keys = _take_keys(
                selected_profile.flat_count,
                flat_lifecycle_count,
                cycle * 251,
                lambda key: not flat_present[key],
            )
            plan.parent_lifecycle_keys = _take_keys(
                selected_profile.parent_count,
                parent_lifecycle_count,
                cycle * 29,
                lambda key: not parent_present[key],
            )
        elif lifecycle_phase == 21:
            plan.lifecycle = Lifecycle.RESTORE_FLAT_AND_PARENTS
            plan.flat_lifecycle_keys = [key for key in range(selected_profile.flat_count) if not flat_present[key]]
            plan.parent_lifecycle_keys = [
                key for key in range(selected_profile.parent_count) if not parent_present[key]
            ]
        elif lifecycle_phase == 40:
            plan.lifecycle = Lifecycle.REMOVE_CHILDREN
            selected_parents = [False] * selected_profile.parent_count
            candidate = cycle * 101
            while len(plan.child_lifecycle_keys) < child_lifecycle_count:
                parent_key = candidate % selected_profile.parent_count
                child_key = (cycle + parent_key) % CHILD_COUNT
                position = _child_index(parent_key, child_key)
                if not selected_parents[parent_key] and parent_present[parent_key] and child_present[position]:
                    plan.child_lifecycle_keys.append((parent_key, child_key))
                    selected_parents[parent_key] = True
                candidate += 1
        elif lifecycle_phase == 41:
            plan.lifecycle = Lifecycle.RESTORE_CHILDREN
            for parent_key in range(selected_profile.parent_count):
                for child_key in range(CHILD_COUNT):
                    if not child_present[_child_index(parent_key, child_key)]:
                        plan.child_lifecycle_keys.append((parent_key, child_key))

        flat_lifecycle_set = set(plan.flat_lifecycle_keys)
        flat_keys = _take_keys(
            selected_profile.flat_count,
            flat_update_count,
            cycle * 257,
            lambda key: not flat_present[key] or key in flat_lifecycle_set,
        )
        plan.flat_updates = [FlatMutation(key, flat_source_value(cycle, key)) for key in flat_keys]

        parent_lifecycle_set = set(plan.parent_lifecycle_keys)
        child_parents = _take_keys(
            selected_profile.parent_count,
            child_update_count,
            cycle * 263,
            lambda key: not parent_present[key] or key in parent_lifecycle_set,
        )
        for parent_key in child_parents:
            child_key = (cycle + parent_key) % CHILD_COUNT
            while (
                not child_present[_child_index(parent_key, child_key)]
                or (parent_key, child_key) in plan.child_lifecycle_keys
            ):
                child_key = (child_key + 1) % CHILD_COUNT
            plan.child_updates.append(
                ChildMutation(parent_key, child_key, child_source_value(cycle, parent_key, child_key))
            )

        if cycle % 1_000 == 50:
            bias += 1
            plan.bias = bias

        for update in plan.flat_updates:
            flat_values[update.key] = update.value
        for update in plan.child_updates:
            child_values[_child_index(update.parent_key, update.child_key)] = update.value

        if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
            for key in plan.flat_lifecycle_keys:
                flat_present[key] = False
            for parent_key in plan.parent_lifecycle_keys:
                parent_present[parent_key] = False
        elif plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
            for key in plan.flat_lifecycle_keys:
                flat_values[key] = flat_source_value(cycle, key)
                flat_present[key] = True
            for parent_key in plan.parent_lifecycle_keys:
                parent_present[parent_key] = True
                for child_key in range(CHILD_COUNT):
                    position = _child_index(parent_key, child_key)
                    child_values[position] = child_source_value(cycle, parent_key, child_key)
                    child_present[position] = True
        elif plan.lifecycle is Lifecycle.REMOVE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                child_present[_child_index(parent_key, child_key)] = False
        elif plan.lifecycle is Lifecycle.RESTORE_CHILDREN:
            for parent_key, child_key in plan.child_lifecycle_keys:
                position = _child_index(parent_key, child_key)
                child_values[position] = child_source_value(cycle, parent_key, child_key)
                child_present[position] = True

        if plan.bias is not None:
            for key in range(selected_profile.flat_count):
                if flat_present[key]:
                    output = stage_b_value(stage_a_value(flat_values[key], key, bias), key, bias)
                    result.expected.add(cycle, Branch.FLAT, 0, key, True, output)
            for parent_key in range(selected_profile.parent_count):
                if not parent_present[parent_key]:
                    continue
                for child_key in range(CHILD_COUNT):
                    position = _child_index(parent_key, child_key)
                    if child_present[position]:
                        output = child_value(child_values[position], parent_key, child_key, bias)
                        result.expected.add(cycle, Branch.CHILD, parent_key, child_key, True, output)
        else:
            for update in plan.flat_updates:
                output = stage_b_value(stage_a_value(update.value, update.key, bias), update.key, bias)
                result.expected.add(cycle, Branch.FLAT, 0, update.key, True, output)
            for update in plan.child_updates:
                output = child_value(update.value, update.parent_key, update.child_key, bias)
                result.expected.add(cycle, Branch.CHILD, update.parent_key, update.child_key, True, output)

            if plan.lifecycle is Lifecycle.REMOVE_FLAT_AND_PARENTS:
                for key in plan.flat_lifecycle_keys:
                    result.expected.add(cycle, Branch.FLAT, 0, key, False, 0)
                for parent_key in plan.parent_lifecycle_keys:
                    for child_key in range(CHILD_COUNT):
                        position = _child_index(parent_key, child_key)
                        if child_present[position]:
                            result.expected.add(cycle, Branch.CHILD, parent_key, child_key, False, 0)
            elif plan.lifecycle is Lifecycle.RESTORE_FLAT_AND_PARENTS:
                for key in plan.flat_lifecycle_keys:
                    output = stage_b_value(stage_a_value(flat_values[key], key, bias), key, bias)
                    result.expected.add(cycle, Branch.FLAT, 0, key, True, output)
                for parent_key in plan.parent_lifecycle_keys:
                    for child_key in range(CHILD_COUNT):
                        position = _child_index(parent_key, child_key)
                        output = child_value(child_values[position], parent_key, child_key, bias)
                        result.expected.add(cycle, Branch.CHILD, parent_key, child_key, True, output)
            elif plan.lifecycle is Lifecycle.REMOVE_CHILDREN:
                for parent_key, child_key in plan.child_lifecycle_keys:
                    result.expected.add(cycle, Branch.CHILD, parent_key, child_key, False, 0)
            elif plan.lifecycle is Lifecycle.RESTORE_CHILDREN:
                for parent_key, child_key in plan.child_lifecycle_keys:
                    output = child_value(
                        child_values[_child_index(parent_key, child_key)], parent_key, child_key, bias
                    )
                    result.expected.add(cycle, Branch.CHILD, parent_key, child_key, True, output)

        result.cycles.append(plan)

    return result


def verify_workload_manifest() -> None:
    """Raise AssertionError if workload generation differs from the C++ manifest."""
    assert initial_flat_value(0) == 1_000_004
    assert initial_child_value(0, 0) == 1_000_036
    assert flat_source_value(0, 0) == 2_000_007
    assert child_source_value(0, 0, 0) == 2_000_069

    smoke = build_workload("smoke", 51)
    assert [update.key for update in smoke.cycles[0].flat_updates] == list(range(7))
    assert smoke.cycles[0].child_updates == [ChildMutation(0, 0, 2_000_069)]
    assert [update.key for update in smoke.cycles[20].flat_updates] == [5, 6, 7, 0, 1, 2, 3]
    assert smoke.cycles[20].flat_lifecycle_keys == [4]
    assert smoke.cycles[20].parent_lifecycle_keys == [0]
    assert smoke.cycles[21].flat_lifecycle_keys == [4]
    assert smoke.cycles[21].parent_lifecycle_keys == [0]
    assert smoke.cycles[40].child_lifecycle_keys == [(0, 0)]
    assert smoke.cycles[41].child_lifecycle_keys == [(0, 0)]
    assert smoke.cycles[50].bias == 2
    assert smoke.expected == EventChecksum(434, 10_494_966_632_268_150_872, 5_460_338_408_372_608_614)

    production = build_workload("25k", 41)
    removal = production.cycles[20]
    assert len(removal.flat_lifecycle_keys) == 250
    assert removal.flat_lifecycle_keys[0] == 5_020 and removal.flat_lifecycle_keys[-1] == 5_269
    assert len(removal.parent_lifecycle_keys) == 25
    assert removal.parent_lifecycle_keys[0] == 580 and removal.parent_lifecycle_keys[-1] == 604
    assert removal.flat_updates[0].key == 5_270 and removal.flat_updates[-1].key == 5_519
    child_removal = production.cycles[40].child_lifecycle_keys
    assert len(child_removal) == 100
    assert child_removal[0] == (40, 0) and child_removal[-1] == (139, 3)

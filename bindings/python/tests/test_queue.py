import copy
import gc
import queue
import threading
import weakref

import pytest

import hg


@pytest.mark.parametrize(
    ("mode_name", "item_type", "expected"),
    [
        ("standard", int, [1, 2]),
        ("elide", int, [2]),
        ("batch", tuple[int, ...], [(1, 2)]),
    ],
)
def test_queue_modes_apply_standard_elided_and_batch_deltas(mode_name, item_type, expected):
    mode = getattr(hg.QueueMode, mode_name)
    output_type = hg.TS[item_type]

    @hg.queue(mode=mode)
    def source(writer: hg.Queue = None) -> output_type:
        assert writer.write(1)
        assert writer.write(2)

    @hg.graph
    def graph() -> output_type:
        return source()

    assert hg.run_graph(graph) == expected


def test_queue_ignores_start_return_when_no_message_is_written():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[int]:
        return 7

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [None]


def test_queue_scalar_inputs_apply_default_arguments():
    @hg.queue
    def source(topic: str = "default", writer: hg.Queue = None) -> hg.TS[int]:
        assert writer.write(len(topic))

    @hg.graph
    def graph() -> hg.TS[int]:
        return source()

    assert hg.run_graph(graph) == [7]


def test_queue_realtime_writer_accepts_delta_keyword_and_closes_on_stop():
    ready = threading.Event()
    received = threading.Event()
    writers = []
    values = []
    failures = []
    stop = hg.StopSource()

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[int]:
        writers.append(copy.copy(writer))
        ready.set()

    @hg.compute_node
    def collect(value: hg.TS[int]) -> None:
        values.append(value.value)
        received.set()

    @hg.graph
    def graph() -> None:
        collect(source())

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    thread = threading.Thread(target=run, daemon=True)
    thread.start()
    try:
        assert ready.wait(5)
        assert writers[0].write(delta=None) is False
        with pytest.raises(TypeError):
            writers[0].write(delta=object())
        assert writers[0].write(delta=42)
        assert received.wait(5)
    finally:
        stop.request_stop()
        thread.join(5)

    assert not thread.is_alive()
    assert failures == []
    assert values == [42]
    assert writers[0].write(delta=43) is False
    assert writers[0].write(delta=object()) is False


def test_queue_copies_bytes_from_a_producer_thread():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[bytes]:
        written = []
        failures = []

        def produce():
            payload = bytearray(b"\x00\x80\xffpayload")
            try:
                written.append(writer.write(memoryview(payload)))
                payload[0] = 1
            except BaseException as error:
                failures.append(error)

        producer = threading.Thread(target=produce)
        producer.start()
        producer.join(5)
        assert not producer.is_alive()
        assert failures == []
        assert written == [True]

    @hg.graph
    def graph() -> hg.TS[bytes]:
        return source()

    assert bytes(hg.run_graph(graph)[0]) == b"\x00\x80\xffpayload"


def test_failed_queue_conversion_commits_no_partial_dictionary_and_writer_recovers():
    ready = threading.Event()
    received = threading.Event()
    writers = []
    values = []
    failures = []
    stop = hg.StopSource()

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[int]]:
        writers.append(writer)
        ready.set()

    @hg.compute_node
    def collect(value: hg.TSD[int, hg.TS[int]]) -> None:
        values.append(dict(value.value))
        received.set()

    @hg.graph
    def graph() -> None:
        collect(source())

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    engine = threading.Thread(target=run, daemon=True)
    engine.start()
    try:
        assert ready.wait(5)
        with pytest.raises(TypeError):
            writers[0].write({1: 10, 2: object()})
        assert writers[0].write({3: 30})
        assert received.wait(5)
    finally:
        stop.request_stop()
        engine.join(5)

    assert not engine.is_alive()
    assert failures == []
    assert values == [{3: 30}]


def test_tuple_scalar_annotation_converts_nested_values():
    @hg.source_node
    def source() -> hg.TS[tuple[dict[int, str], ...]]:
        return [{1: "one"}]

    @hg.graph
    def graph() -> hg.TS[tuple[dict[int, str], ...]]:
        return source()

    assert hg.run_graph(graph) == [({1: "one"},)]


def test_queue_missing_dictionary_key_removal_is_a_no_op():
    received = []
    writers = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[int]]:
        writers.append(writer)
        assert writer.write({1: 10})

    @hg.compute_node
    def collect(values: hg.TSD[int, hg.TS[int]]) -> None:
        received.append(dict(values.value))
        if len(received) == 1:
            assert writers[0].write({2: hg.REMOVE})

    @hg.graph
    def graph() -> None:
        collect(source())

    hg.run_graph(graph)
    assert received == [{1: 10}]


def test_batch_queue_projects_nested_tuples_into_a_batch_list():
    @hg.queue(mode=hg.QueueMode.batch)
    def source(writer: hg.Queue = None) -> hg.TS[tuple[tuple[int, ...], ...]]:
        assert writer.write([1, 2])
        assert writer.write([3])

    @hg.graph
    def graph() -> hg.TS[tuple[tuple[int, ...], ...]]:
        return source()

    assert hg.run_graph(graph) == [((1, 2), (3,))]


def test_batch_invalidation_breaks_the_batch_and_pending_values_resume_later():
    @hg.queue(mode=hg.QueueMode.batch)
    def source(writer: hg.Queue = None) -> hg.TS[tuple[int, ...]]:
        assert writer.write(10)
        assert writer.write(hg.INVALID)
        assert writer.write(20)

    @hg.graph
    def graph() -> hg.TS[tuple[int, ...]]:
        return source()

    assert hg.run_graph(graph) == [(10,), None, (20,)]


def test_batch_queue_round_trips_through_tsl_tsb_and_tsd_leaves():
    @hg.schema
    class Pair:
        left: hg.TS[tuple[int, ...]]
        right: hg.TS[tuple[int, ...]]

    @hg.queue(mode=hg.QueueMode.batch)
    def list_source(writer: hg.Queue = None) -> hg.TSL[hg.TS[tuple[int, ...]], 2]:
        assert writer.write({0: 1})
        assert writer.write({0: 2})
        assert writer.write({1: 3})

    @hg.queue(mode=hg.QueueMode.batch)
    def bundle_source(writer: hg.Queue = None) -> hg.TSB[Pair]:
        assert writer.write({"left": 4})
        assert writer.write({"left": 5, "right": 6})

    @hg.queue(mode=hg.QueueMode.batch)
    def dictionary_source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[tuple[int, ...]]]:
        assert writer.write({7: 7})
        assert writer.write({7: 8, 9: 9})

    @hg.compute_node
    def total(
        values: hg.TSL[hg.TS[tuple[int, ...]], 2],
        pair: hg.TSB[Pair],
        dictionary: hg.TSD[int, hg.TS[tuple[int, ...]]],
    ) -> hg.TS[int]:
        return (
            sum(values[0].value)
            + sum(values[1].value)
            + sum(pair.left.value)
            + sum(pair.right.value)
            + sum(sum(item) for item in dictionary.value.values())
        )

    @hg.graph
    def graph() -> hg.TS[int]:
        return total(list_source(), bundle_source(), dictionary_source())

    assert hg.run_graph(graph) == [45]


def test_queue_projects_tss_additions_and_removals():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TSS[int]:
        assert writer.write({1, 2})
        assert writer.write({3, hg.Removed(1)})

    @hg.compute_node
    def total(values: hg.TSS[int]) -> hg.TS[int]:
        return sum(values.value)

    @hg.graph
    def graph() -> hg.TS[int]:
        return total(source())

    assert hg.run_graph(graph) == [3, 5]


def test_queue_projects_tsd_invalidation_removal_and_recreation():
    snapshots = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[int]]:
        assert writer.write({1: 10, 2: 20})
        assert writer.write({1: hg.INVALID})
        assert writer.write({1: hg.REMOVE})
        assert writer.write({1: 30})

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[int]]) -> None:
        snapshot = {}
        for key in (1, 2):
            if key in values:
                item = values[key]
                snapshot[key] = item.value if item.valid else None
        snapshots.append(snapshot)

    @hg.graph
    def graph() -> None:
        inspect(source())

    hg.run_graph(graph)
    assert snapshots == [{1: 10, 2: 20}, {1: None, 2: 20}, {2: 20}, {1: 30, 2: 20}]


def test_batch_dictionary_removal_breaks_the_batch_before_recreation():
    snapshots = []

    @hg.queue(mode=hg.QueueMode.batch)
    def source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[tuple[int, ...]]]:
        assert writer.write({1: 10})
        assert writer.write({1: hg.REMOVE})
        assert writer.write({1: 20})

    @hg.compute_node
    def inspect(values: hg.TSD[int, hg.TS[tuple[int, ...]]]) -> None:
        snapshots.append({key: tuple(value) for key, value in values.value.items()})

    @hg.graph
    def graph() -> None:
        inspect(source())

    hg.run_graph(graph)
    assert snapshots == [{1: (10,)}, {}, {1: (20,)}]


def test_queue_start_and_stop_callbacks_select_scalar_state_and_queue_parameters():
    events = []
    retained = []
    states = []

    @hg.queue
    def source(topic: str = "default", writer: hg.Queue = None, state: hg.State[list] = None) -> hg.TS[int]:
        state.append(("start", topic))
        events.append(("start", topic))
        states.append(state)
        retained.append(copy.copy(writer))
        assert writer.write(len(topic))

    @source.stop
    def stop(*, state: hg.State[list] = None, topic: str, writer: hg.Queue):
        state.append(("stop", topic))
        events.append(("stop", topic, writer.write(delta=999)))

    @hg.graph
    def graph() -> hg.TS[int]:
        return source(topic="orders")

    assert hg.run_graph(graph) == [6]
    assert events == [("start", "orders"), ("stop", "orders", False)]
    assert states == [[("start", "orders"), ("stop", "orders")]]
    assert retained[0].write(delta=object()) is False


def test_queue_requires_its_writer_and_time_series_return_annotation():
    with pytest.raises(TypeError, match="exactly one Queue"):

        @hg.queue
        def missing_writer() -> hg.TS[int]:
            pass

    with pytest.raises(TypeError, match="return annotation"):

        @hg.queue
        def missing_output(writer: hg.Queue = None):
            pass


def test_queue_lifecycle_is_start_only_and_rejects_invalidation():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[int]:
        pass

    with pytest.raises(TypeError, match="start callback is already defined"):

        @source.start
        def duplicate_start():
            pass

    with pytest.raises(TypeError, match="sources cannot register invalidation"):

        @source.on_invalidation
        def invalidation():
            pass


def test_queue_mode_must_be_a_queue_mode():
    with pytest.raises(TypeError, match="mode must be a hg.QueueMode"):
        hg.queue(mode="batch")


def test_queue_output_validation_rejects_nonordinary_and_invalid_batch_types():
    @hg.queue
    def final_source(writer: hg.Queue = None) -> hg.TS[int, hg.final]:
        pass

    @hg.graph
    def final_graph() -> hg.TS[int, hg.final]:
        return final_source()

    with pytest.raises(ValueError, match="queue outputs require the ordinary guarantee"):
        hg.run_graph(final_graph)

    @hg.queue(mode=hg.QueueMode.batch)
    def scalar_batch_source(writer: hg.Queue = None) -> hg.TS[int]:
        pass

    @hg.graph
    def scalar_batch_graph() -> hg.TS[int]:
        return scalar_batch_source()

    with pytest.raises(ValueError, match="batch queue leaves require a scalar list"):
        hg.run_graph(scalar_batch_graph)

    @hg.queue(mode=hg.QueueMode.batch)
    def set_batch_source(writer: hg.Queue = None) -> hg.TSS[int]:
        pass

    @hg.graph
    def set_batch_graph() -> hg.TSS[int]:
        return set_batch_source()

    with pytest.raises(ValueError, match="batch queues do not support TSS outputs"):
        hg.run_graph(set_batch_graph)


def test_reference_queue_output_rejects_python_literal_writes():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.REF[hg.TS[int]]:
        writer.write(1)

    @hg.graph
    def graph() -> hg.REF[hg.TS[int]]:
        return source()

    with pytest.raises(ValueError, match="unsupported Python time-series literal type"):
        hg.run_graph(graph)


def test_copied_queue_writers_preserve_each_writers_order_when_used_concurrently():
    writer_ready = threading.Event()
    received = threading.Event()
    writers = []
    values = []
    failures = []
    stop = hg.StopSource()
    count_per_writer = 16

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[int]:
        writers.extend((copy.copy(writer), copy.copy(writer)))
        writer_ready.set()

    @hg.compute_node
    def collect(value: hg.TS[int]) -> None:
        values.append(value.value)
        if len(values) == count_per_writer * len(writers):
            received.set()

    @hg.graph
    def graph() -> None:
        collect(source())

    def produce(writer, start):
        try:
            for value in range(start, start + count_per_writer):
                if not writer.write(value):
                    failures.append(RuntimeError("queue rejected a producer write"))
                    return
        except BaseException as error:
            failures.append(error)

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    engine = threading.Thread(target=run, daemon=True)
    engine.start()
    producers = []
    try:
        assert writer_ready.wait(5)
        producers = [
            threading.Thread(target=produce, args=(writers[0], 0), daemon=True),
            threading.Thread(target=produce, args=(writers[1], 100), daemon=True),
        ]
        for producer in producers:
            producer.start()
        for producer in producers:
            producer.join(5)
        assert all(not producer.is_alive() for producer in producers)
        assert received.wait(5)
    finally:
        stop.request_stop()
        for producer in producers:
            producer.join(5)
        engine.join(5)

    assert not engine.is_alive()
    assert failures == []
    assert [value for value in values if value < 100] == list(range(count_per_writer))
    assert [value for value in values if value >= 100] == list(range(100, 100 + count_per_writer))


def test_queue_rejects_reentrant_write_and_copy_during_mapping_conversion():
    ready = threading.Event()
    received = threading.Event()
    writers = []
    nested_errors = []
    values = []
    failures = []
    stop = hg.StopSource()

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TSD[int, hg.TS[int]]:
        writers.append(writer)
        ready.set()

    @hg.compute_node
    def collect(value: hg.TSD[int, hg.TS[int]]) -> None:
        values.extend(value.value.values())
        received.set()

    @hg.graph
    def graph() -> None:
        collect(source())

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    class ReentrantMapping(dict):
        def items(self):
            for name, operation in (
                ("write", lambda: writers[0].write({9: 9})),
                ("copy", lambda: copy.copy(writers[0])),
            ):
                try:
                    operation()
                except RuntimeError as error:
                    nested_errors.append((name, str(error)))
                else:
                    raise AssertionError("reentrant queue operation was accepted")
            return super().items()

    engine = threading.Thread(target=run, daemon=True)
    engine.start()
    try:
        assert ready.wait(5)
        assert writers[0].write(ReentrantMapping({1: 7}))
        assert received.wait(5)
    finally:
        stop.request_stop()
        engine.join(5)

    assert not engine.is_alive()
    assert failures == []
    assert values == [7]
    assert [name for name, _ in nested_errors] == ["write", "copy"]
    assert "same handle" in nested_errors[0][1]
    assert "while it is writing" in nested_errors[1][1]


def test_queue_output_is_restricted_to_outer_runtime_graphs():
    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[int]:
        assert writer.write(1)

    @hg.graph
    def branch() -> hg.TS[int]:
        return source()

    @hg.graph
    def outer(key: hg.TS[int]) -> hg.TS[int]:
        return hg.switch_(key, {1: branch})

    with pytest.raises(RuntimeError, match="only an outer runtime graph can create a queue"):
        hg.run_graph(outer, [1])


def test_python_object_queue_payloads_are_released_after_graph_teardown():
    class Value:
        pass

    references = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[Value]:
        value = Value()
        references.append(weakref.ref(value))
        assert writer.write(value)

    @hg.compute_node
    def discard(value: hg.TS[Value]) -> None:
        pass

    @hg.graph
    def graph() -> None:
        discard(source())

    hg.run_graph(graph)
    gc.collect()
    assert references[0]() is None


def test_unread_python_object_queue_payloads_are_released_after_early_stop():
    class Value:
        pass

    stop = hg.StopSource()
    ready = threading.Event()
    references = []
    received = []
    failures = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[Value]:
        for _ in range(2):
            value = Value()
            references.append(weakref.ref(value))
            assert writer.write(value)
            del value
        ready.set()

    @hg.compute_node
    def collect(value: hg.TS[Value]) -> None:
        received.append(True)
        stop.request_stop()

    @hg.graph
    def graph() -> None:
        collect(source())

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    engine = threading.Thread(target=run, daemon=True)
    engine.start()
    try:
        assert ready.wait(5)
        engine.join(5)
    finally:
        stop.request_stop()
        engine.join(5)

    assert not engine.is_alive()
    assert failures == []
    assert received == [True]
    gc.collect()
    assert [reference() for reference in references] == [None, None]


def test_python_object_queue_payload_is_released_during_realtime_execution():
    class Value:
        def __init__(self, released):
            self.released = released

        def __del__(self):
            self.released.set()

    stop = hg.StopSource()
    ready = threading.Event()
    received = queue.Queue()
    writers = []
    references = []
    failures = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[Value]:
        writers.append(copy.copy(writer))
        ready.set()

    @hg.compute_node
    def observe(value: hg.TS[Value]) -> None:
        received.put(True)

    @hg.graph
    def graph() -> None:
        observe(source())

    def run():
        try:
            hg.run_graph(graph, engine=hg.EvaluationEngine.realtime, stop_token=stop.get_token())
        except BaseException as error:
            failures.append(error)

    engine = threading.Thread(target=run, daemon=True)
    engine.start()
    released = threading.Event()
    try:
        assert ready.wait(5)

        first = Value(released)
        references.append(weakref.ref(first))
        assert writers[0].write(first)
        del first
        assert received.get(timeout=5)

        second = Value(threading.Event())
        assert writers[0].write(second)
        del second
        assert received.get(timeout=5)

        assert released.wait(5)
        assert engine.is_alive()
        assert not stop.stop_requested()
        assert references[0]() is None
    finally:
        stop.request_stop()
        engine.join(5)

    assert not engine.is_alive()
    assert failures == []


def test_queue_handle_closes_and_releases_payload_after_startup_failure():
    class Value:
        pass

    retained = []
    references = []

    @hg.queue
    def source(writer: hg.Queue = None) -> hg.TS[Value]:
        retained.append(copy.copy(writer))
        value = Value()
        references.append(weakref.ref(value))
        assert writer.write(value)
        raise ValueError("queue startup failed")

    @hg.graph
    def graph() -> hg.TS[Value]:
        return source()

    with pytest.raises(ValueError, match="queue startup failed"):
        hg.run_graph(graph)

    assert retained[0].write(delta=object()) is False
    gc.collect()
    assert references[0]() is None

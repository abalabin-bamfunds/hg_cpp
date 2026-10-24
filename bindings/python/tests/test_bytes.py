import gc
import hashlib
import struct

import pytest

import bg
from bg._bg_test_extension import bytes_conversion_addresses, const_, round_trip_bytes


def test_bytes_export_is_readonly_and_outlives_the_graph():
    @bg.source_node
    def source() -> bg.TS[bytes]:
        return b"\x00payload\xff"

    @bg.graph
    def graph() -> bg.TS[bytes]:
        return source()

    result = bg.run_graph(graph)[0]
    gc.collect()

    assert isinstance(result, memoryview)
    assert result.readonly
    assert result.format == "B"
    assert result.shape == (9,)
    assert result.strides == (1,)
    assert bytes(result) == b"\x00payload\xff"


def test_bytes_export_and_full_memoryview_import_share_native_storage():
    exported = round_trip_bytes(b"contiguous")
    assert bytes(exported) == b"contiguous"
    assert bytes_conversion_addresses(exported) == (True, True, True)

    assert bytes_conversion_addresses(b"python bytes") == (False, True, True)

    high_bits = round_trip_bytes(b"\x00\x80\xff")
    assert bytes(high_bits) == b"\x00\x80\xff"
    assert bytes(round_trip_bytes(b"")) == b""
    assert bytes_conversion_addresses(memoryview(b"read-only"))[0] is False


def test_memoryview_is_accepted_by_bytes_like_python_consumers():
    payload = round_trip_bytes(struct.pack(">I", 0x80FF00AA))
    assert struct.unpack(">I", payload) == (0x80FF00AA,)
    assert hashlib.sha256(payload).digest() == hashlib.sha256(b"\x80\xff\x00\xaa").digest()


def test_memoryview_slices_and_external_buffers_are_copied_exactly():
    source = bytearray(b"mutable payload")
    external = memoryview(source)
    copied = round_trip_bytes(external)
    source[0] = ord("M")
    assert bytes(copied) == b"mutable payload"
    assert bytes_conversion_addresses(external)[0] is False

    original = round_trip_bytes(b"slice payload")
    sliced = memoryview(original)[1:-1]
    copied_slice = round_trip_bytes(sliced)
    assert bytes(copied_slice) == b"lice payloa"
    assert bytes_conversion_addresses(sliced)[0] is False

    retained_slice = memoryview(original)[1:]
    original.release()
    del original
    gc.collect()
    assert bytes(retained_slice) == b"lice payload"


def test_noncontiguous_memoryview_is_rejected():
    with pytest.raises(TypeError, match="expected bytes"):
        round_trip_bytes(bytearray(b"not a memoryview"))

    with pytest.raises(TypeError, match="contiguous"):
        round_trip_bytes(memoryview(b"abcdef")[::2])

    released = memoryview(b"released")
    released.release()
    with pytest.raises(ValueError, match="released"):
        round_trip_bytes(released)


def test_memoryview_literal_infers_bg_bytes_type():
    value = memoryview(b"literal")

    @bg.graph
    def constant() -> bg.TS[bytes]:
        return const_(value=value)

    assert bytes(bg.run_graph(constant)[0]) == b"literal"


def test_run_graph_classifies_bytes_and_memoryview_as_single_bytes_values():
    @bg.graph
    def identity(value: bg.TS[bytes]) -> bg.TS[bytes]:
        return value

    assert bytes(bg.run_graph(identity, b"initial")[0]) == b"initial"
    assert bytes(bg.run_graph(identity, memoryview(b"view"))[0]) == b"view"
    assert [bytes(value) for value in bg.run_graph(identity, [b"first", b"second"])] == [b"first", b"second"]

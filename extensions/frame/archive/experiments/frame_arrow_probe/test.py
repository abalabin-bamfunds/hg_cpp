import ctypes
import sys

import pyarrow as pa


library = ctypes.CDLL(sys.argv[1])
library.probe_init.restype = ctypes.c_int
library.probe_destroy.restype = None
library.probe_live_count.restype = ctypes.c_size_t
library.probe_null_slot_is_unconstructed.restype = ctypes.c_int
library.probe_edge_null_run_is_unconstructed.restype = ctypes.c_int
library.probe_edge_live_count.restype = ctypes.c_size_t
library.probe_edge_values_address.restype = ctypes.c_size_t
library.probe_edge_values_length.restype = ctypes.c_size_t
library.probe_chunk_count.argtypes = [ctypes.c_int]
library.probe_chunk_count.restype = ctypes.c_size_t
library.probe_chunk_length.argtypes = [ctypes.c_int, ctypes.c_size_t]
library.probe_chunk_length.restype = ctypes.c_size_t
library.probe_values_address.argtypes = [ctypes.c_int, ctypes.c_size_t]
library.probe_values_address.restype = ctypes.c_size_t
library.probe_validity_address.argtypes = [ctypes.c_int, ctypes.c_size_t]
library.probe_validity_address.restype = ctypes.c_size_t
library.probe_validity_size.argtypes = [ctypes.c_int, ctypes.c_size_t]
library.probe_validity_size.restype = ctypes.c_size_t
library.probe_null_count.argtypes = [ctypes.c_int, ctypes.c_size_t]
library.probe_null_count.restype = ctypes.c_size_t


def make_column(column, nullable):
    chunks = []
    lengths = []
    for chunk_index in range(library.probe_chunk_count(column)):
        length = library.probe_chunk_length(column, chunk_index)
        values_address = library.probe_values_address(column, chunk_index)
        values = pa.foreign_buffer(values_address, length * 8)

        validity_address = library.probe_validity_address(column, chunk_index)
        validity_size = library.probe_validity_size(column, chunk_index)
        validity = pa.foreign_buffer(validity_address, validity_size) if validity_address else None
        null_count = library.probe_null_count(column, chunk_index)
        array = pa.Array.from_buffers(pa.int64(), length, [validity, values], null_count=null_count)

        assert array.buffers()[1].address == values_address
        if validity_address:
            assert array.buffers()[0].address == validity_address
        elif nullable:
            assert null_count == 0

        chunks.append(array)
        lengths.append(length)

    return pa.chunked_array(chunks), lengths


assert library.probe_init() == 0
try:
    assert library.probe_null_slot_is_unconstructed()
    assert library.probe_edge_null_run_is_unconstructed()
    ids, id_lengths = make_column(0, False)
    measurements, measurement_lengths = make_column(1, True)
    table = pa.Table.from_arrays([ids, measurements], names=["id", "measurement"])

    expected_positions = [0, 1, 3, 4, 7, 8, 9, 10, 22, 23, 24, 25, 40]
    expected_ids = [10_000 + position for position in expected_positions]
    expected_measurements = [None if position == 9 else 20_000 + position for position in expected_positions]

    assert table.num_rows == library.probe_live_count() == len(expected_positions)
    assert table.column("id").to_pylist() == expected_ids
    assert table.column("measurement").to_pylist() == expected_measurements
    assert id_lengths == [2, 2, 1, 3, 2, 2, 1]
    assert measurement_lengths == id_lengths
    assert table.column("measurement").null_count == 1

    synthetic_values = pa.py_buffer(bytes(2 * 8))
    synthetic_validity = pa.py_buffer(bytes(1))
    null_run = pa.Array.from_buffers(
        pa.int64(), 2, [synthetic_validity, synthetic_values], null_count=2
    )
    assert null_run.buffers()[0].size == 1
    assert null_run.buffers()[1].size == 2 * 8

    edge_length = library.probe_edge_values_length()
    edge_address = library.probe_edge_values_address()
    edge_values = pa.foreign_buffer(edge_address, edge_length * 8)
    edge_span = pa.Array.from_buffers(pa.int64(), edge_length, [None, edge_values], null_count=0)
    assert edge_span.buffers()[1].address == edge_address

    edge_column = pa.chunked_array([null_run, edge_span])
    edge_table = pa.Table.from_arrays([edge_column], names=["edge"])
    assert library.probe_edge_live_count() == 4
    assert edge_column.num_chunks == 2
    assert [len(chunk) for chunk in edge_column.chunks] == [2, 2]
    assert edge_table.num_rows == 4
    assert edge_table.column("edge").to_pylist() == [None, None, 30_008, 30_009]

    print(f"PyArrow {pa.__version__}")
    print(f"live rows: {table.num_rows}")
    print(f"chunks per column: {id_lengths}")
    print("row order, deleted-row splits, block-boundary splits, and null validity: PASS")
    print("value buffers shared with sparse_bunch: PASS")
    print("null validity bitmap exported as a copied Arrow bitmap: PASS")
    print("null cell unconstructed and not dereferenced: PASS")
    print("null-only prefix uses a synthetic Arrow values buffer: PASS")
    print("constructed suffix remains zero-copy across the block boundary: PASS")
    print("one live run exported as two chunks: PASS")
finally:
    library.probe_destroy()

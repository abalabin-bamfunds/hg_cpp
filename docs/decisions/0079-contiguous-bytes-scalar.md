# Contiguous byte scalar

`bytes` represents one immutable contiguous binary payload. `List<byte>` represents
a collection of byte scalar items and does not guarantee a contiguous byte buffer.
The distinct scalar lets network and codec code pass one payload as a span while
scalar storage retains its bytes by reference.

Construction from a borrowed span copies into shared storage. `BytesBuilder` lets
producers fill owned storage before transferring it to a scalar value.

Python exports `bytes` as a read-only `memoryview` that retains the native
storage for the view's lifetime. Reimporting a full BG-exported view shares
that storage; partial views and external memoryviews copy their visible
contents. See [Python graph authoring](../python.md#graph-authoring) for the
boundary contract.

See [the scalar API](../scalar-api.md) for the storage and builder contract.

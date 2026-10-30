# Key-addressed frame row literals

TSF row literals identify rows with the schema's user-defined key columns.
Existing keys patch rows, absent keys insert rows, and `REMOVE` names a key to
remove. Unkeyed row literals append. Key cells, including null cells, use the
same uniqueness rules as native keyed storage.

Internal row IDs remain separate from user keys. They can be recycled and
belong to one row domain, so literals do not expose them. Native storage
retains removed key values through the removal cycle so row-removal deltas can
be identified by the same user key.

See [TSF row updates](../../extensions/frame/README.md#tsf-row-updates) for the
literal syntax and update behavior.

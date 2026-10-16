# Kuzu 0.11 / storage 39 read-only recovery fixtures

These synthetic files were generated on Windows x86_64 by the unmodified
`kuzu-0.11.0-cp312-cp312-win_amd64.whl`, using `generate.py`. Their headers were
not edited. `provenance.json` records the generated file hashes and sizes.
The generator creates each database independently. This historical format does
not carry the current engine UUID/checksum framing; independent generation does
not prove foreign-WAL detection.

Run the generator with an installed **Kuzu 0.11.0** package directory:

```sh
python generate.py --package /path/to/kuzu-0.11-package --output /new/output
```

- `clean.db`: checkpointed 260 nodes and 518 directed, parallel relationships.
- `wal.db` plus `wal.db.wal`: the same shape with an additional node and edge
  committed only in WAL. The writer exits without database destructors.
- Values include page-spanning UTF-8, combining characters, lists, dates,
  microsecond timestamps, NULL, empty strings and opaque extraction metadata.

The original old reader independently read both families, including the WAL-only
record, and left their bytes unchanged. The proposed regression asserts every node, primary-key lookup, both edge
directions, parallel occurrences and source bytes. It currently FAILS with the
experimental header-only current reader: clean data reaches an incompatible
column codec, and the WAL family fails checksum-framing admission. The current
reader is therefore NOT validated for either fixture. Negative tests alter
disposable copies only.

The old reader result establishes fixture provenance, not a compatible new
reader or complete WAL-recovery safety. This experiment is rejected for product
integration. A separate pinned old-reader process is required. These are **not**
an old-writer pending-checkpoint/shadow fixture. That input remains unsupported;
the existing C++ interruption seam separately verifies read-only refusal and
preservation before ordinary current-writer checkpoint recovery.

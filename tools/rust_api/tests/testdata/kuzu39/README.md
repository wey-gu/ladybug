# Kuzu 0.11 / storage 39 read-only recovery fixtures

These synthetic files were generated on Windows x86_64 by the unmodified
`kuzu-0.11.0-cp312-cp312-win_amd64.whl`, using `generate.py`. Their headers were
not edited. `provenance.json` records the generated file hashes and sizes.
The generator creates each database independently, so their UUIDs differ.

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
record, and left their bytes unchanged. The regression checks every node, primary
key lookup, both edge directions, parallel occurrences and source bytes after
closing/reopening the new reader. Negative tests alter disposable copies only.

This is evidence for clean checkpoints and ordinary committed WAL. It is **not**
an old-writer pending-checkpoint/shadow fixture. That input remains unsupported;
the existing C++ interruption seam separately verifies read-only refusal and
preservation before ordinary current-writer checkpoint recovery.

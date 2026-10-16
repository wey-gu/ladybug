"""Generate synthetic compatibility evidence with the unmodified Kuzu 0.11 writer."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

p = argparse.ArgumentParser()
p.add_argument('--package', required=True)
p.add_argument('--output', required=True)
p.add_argument('--worker', choices=['clean', 'wal'])
a = p.parse_args()
out = Path(a.output)
sys.path.insert(0, a.package)
import kuzu

if a.worker:
    db = kuzu.Database(str(out / (a.worker + '.db')), buffer_pool_size=128 * 1024 * 1024,
                       max_num_threads=2, auto_checkpoint=False)
    c = kuzu.Connection(db)
    c.execute('CREATE NODE TABLE Item(id STRING PRIMARY KEY, text STRING, tags STRING[], number INT64, day DATE, moment TIMESTAMP)')
    c.execute('CREATE REL TABLE Link(FROM Item TO Item, kind STRING, raw_context STRING, extraction_metadata STRING)')
    for i in range(260):
        c.execute('CREATE (:Item {id:$id,text:$text,tags:$tags,number:$number,day:$day,moment:$moment})',
                  {'id': f'item-{i:04}', 'text': ('旧库真实写入 🌍' * 1100 if i == 259 else f'body-{i}'),
                   'tags': ['中文', '', 'e\u0301'], 'number': i,
                   'day': datetime.date(2025, 7, 14), 'moment': datetime.datetime(2025, 7, 14, 12, 13, 14, 123456)})
    for i in range(259):
        for k in range(2):
            c.execute('MATCH (s:Item {id:$source}),(t:Item {id:$target}) CREATE (s)-[:Link {kind:$kind,raw_context:$context,extraction_metadata:$metadata}]->(t)',
                      {'source': f'item-{i:04}', 'target': f'item-{i+1:04}', 'kind': 'parallel',
                       'context': '上下文' if k else None, 'metadata': 'opaque:not JSON' if k else ''})
    c.execute('CHECKPOINT')
    if a.worker == 'wal':
        c.execute("CREATE (:Item {id:'wal-only',text:'committed WAL record',tags:[],number:999})")
        c.execute("MATCH (s:Item {id:'item-0259'}),(t:Item {id:'wal-only'}) CREATE (s)-[:Link {kind:'wal-edge',raw_context:'WAL context'}]->(t)")
        # No destructor checkpoint: the committed data must remain only in WAL.
        os._exit(0)
    c.close()
    db.close()
else:
    out.mkdir(parents=True, exist_ok=False)
    for mode in ['clean', 'wal']:
        subprocess.run([sys.executable, __file__, '--package', a.package,
                        '--output', str(out), '--worker', mode], check=True)
    report = {'writer_version': kuzu.__version__, 'python': sys.version,
              'platform': sys.platform, 'node_count_clean': 260,
              'edge_count_clean': 518, 'node_count_wal': 261, 'edge_count_wal': 519,
              'files': {f.name: {'size': f.stat().st_size,
                               'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
                        for f in out.iterdir() if f.is_file()}}
    (out / 'provenance.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))

#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:?Provide the configured build directory}
runner="$build_dir/test/runner/e2e_test"
evidence="$build_dir/empty-index-evidence"
source_file=src/storage/index/hash_index.cpp
fixture=transaction/create_node/empty_string_index_checkpoint.test
mkdir -p "$evidence"
backup=$(mktemp)
cp "$source_file" "$backup"
trap 'cp "$backup" "$source_file"; rm -f "$backup"' EXIT

run_green() {
    local phase=$1
    "$runner" "$fixture" --gtest_output="xml:$evidence/$phase.xml" > "$evidence/$phase.log" 2>&1
    cat "$evidence/$phase.log"
    python3 - "$evidence/$phase.xml" <<'PYTEST'
import sys
import xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
assert root.get("tests") == "2", "Both fixture cases must execute"
assert root.get("failures") == "0", "Fixture must pass"
assert root.get("disabled") == "0", "Neither fixture case may be disabled"
PYTEST
}

run_green fixed
python3 - "$source_file" <<'PYMUTANT'
import sys
from pathlib import Path
path = Path(sys.argv[1])
source = path.read_text()
fixed = """if (overflowFile &&
        storageInfo->cast<PrimaryKeyIndexStorageInfo>().firstHeaderPage != INVALID_PAGE_IDX) {"""
assert source.count(fixed) == 1, "Expected exactly one paired-header guard"
path.write_text(source.replace(fixed, "if (overflowFile) {"))
PYMUTANT
cmake --build "$build_dir" --target e2e_test -j 8 > "$evidence/old-production-build.log" 2>&1
set +e
"$runner" "$fixture" > "$evidence/old-production.log" 2>&1
old_status=$?
set -e
cat "$evidence/old-production.log"
if [ "$old_status" -eq 0 ]; then
    echo "Old production checkpoint unexpectedly passed" >&2
    exit 1
fi
grep -F 'hashIndexStorageInfo.overflowHeaderPage == INVALID_PAGE_IDX' "$evidence/old-production.log"
cp "$backup" "$source_file"
cmp "$backup" "$source_file"
cmake --build "$build_dir" --target e2e_test -j 8 > "$evidence/restored-build.log" 2>&1
run_green restored

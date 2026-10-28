#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
case "${1:-}" in
'') tests='t0000-basic.sh t0001-init.sh t0002-gitfile.sh t0451-memory.sh t0452-memory-safety.sh t1500-rev-parse.sh t2200-add-update.sh t2400-worktree-add.sh t7508-status.sh' ;;
--native-only) tests='t0451-memory.sh t0452-memory-safety.sh' ;;
*) echo >&2 'usage: scripts/test-memory.sh [--native-only]'; exit 64 ;;
esac
cd "$root/t"
for test_file in $tests
do
	sh "$test_file"
done

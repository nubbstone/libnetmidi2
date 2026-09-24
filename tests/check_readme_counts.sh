#!/bin/sh
# Assert the README's per-suite test table still matches reality.
#
# The table exists to tell a reader what each suite is worth. A stale count makes it
# worth less than no table at all, and nothing else in CI would notice: the suites
# would still pass, the README would simply be lying. Run from the repo root with the
# binaries already built in ./build.
set -eu

README=README.md
BUILD=${1:-build}
fail=0
total=0

# Rows look like:  | `suite_name` | 42 | ... | ... |
sed -n 's/^| `\([a-z_]*\)` | \([0-9]*\) |.*/\1 \2/p' "$README" | while :; do
    read -r name claimed || break
    [ -n "${name:-}" ] || continue
    bin="$BUILD/nm2_$name"
    if [ ! -x "$bin" ]; then
        echo "  MISSING  $bin (table names a suite that does not build)"
        exit 1
    fi
    actual=$("$bin" 2>&1 | grep -c '^  \(OK\|FAIL\)' || true)
    if [ "$actual" != "$claimed" ]; then
        echo "  STALE    $name: README says $claimed, binary reports $actual"
        exit 1
    fi
    printf '  ok       %-22s %s\n' "$name" "$claimed"
done

# The prose total has to agree with the rows as well.
rows=$(sed -n 's/^| `[a-z_]*` | \([0-9]*\) |.*/\1/p' "$README" | awk '{s+=$1} END {print s+0}')
prose=$(sed -n 's/^\([0-9]*\) checks across nine suites.*/\1/p' "$README" | head -1)
if [ "$rows" != "$prose" ]; then
    echo "  STALE    table sums to $rows but the prose claims $prose"
    exit 1
fi
echo "  ok       table sums to $rows, matching the prose"

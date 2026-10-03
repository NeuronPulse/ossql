#!/usr/bin/env bash
# ossql smoke tests. Invoked by CTest with the built binary path as $1.
# Runs a handful of read-only queries and asserts on the output. No daemon,
# no state — just exercises each table and the new columns.
set -u
OSQL="${1:-./build/ossql}"
fail=0

run() { "$OSQL" "$@" 2>&1; }

# ok desc sql needle  -- output must contain needle (exit 0).
ok() {
  local desc="$1" sql="$2" needle="$3" out
  out=$(run -headers off "$sql") || { echo "FAIL(exit): $desc"; fail=1; return; }
  if printf '%s' "$out" | grep -qF -- "$needle"; then
    echo "ok: $desc"
  else
    echo "FAIL: $desc"; echo "  got: $out"; fail=1
  fi
}

# ok_int_gt desc sql min -- output (first token) is an integer > min.
ok_int_gt() {
  local desc="$1" sql="$2" min="$3" out n
  out=$(run -headers off "$sql") || { echo "FAIL(exit): $desc"; fail=1; return; }
  n=$(printf '%s' "$out" | tr -d ' \t\r' | head -1)
  if [[ "$n" =~ ^[0-9]+$ ]] && [ "$n" -gt "$min" ]; then
    echo "ok: $desc"
  else
    echo "FAIL: $desc (got '$n')"; fail=1
  fi
}

ok_int_gt "system has one row"          "SELECT count(*) FROM system"                      0
ok_int_gt "system mem_total > 0"        "SELECT count(*) FROM system WHERE mem_total > 0"  0
ok_int_gt "proc has rows"               "SELECT count(*) FROM proc"                         0
ok_int_gt "proc io_read column works"   "SELECT count(*) FROM (SELECT io_read FROM proc LIMIT 1)" 0
ok_int_gt "proc rlimit_nofile works"    "SELECT count(*) FROM (SELECT rlimit_nofile FROM proc LIMIT 1)" 0
ok_int_gt "passwd resolvable"           "SELECT count(*) FROM passwd"                       0
ok_int_gt "net has rows"                "SELECT count(*) FROM net"                          0
ok_int_gt "net has unix sockets"        "SELECT count(*) FROM net WHERE proto='unix'"       0

# cgroup: query must succeed; rows may be 0 if cgroup is unmounted.
if run -headers off "SELECT count(*) FROM cgroup LIMIT 1" >/dev/null 2>&1; then
  echo "ok: cgroup queryable"
else
  echo "FAIL(exit): cgroup queryable"; fail=1
fi

# -output snapshot round-trips into a real sqlite file if sqlite3 is available.
tmp=$(mktemp /tmp/ossql_snap.XXXXXX.db)
if command -v sqlite3 >/dev/null 2>&1; then
  run -output "$tmp" "SELECT pid FROM proc LIMIT 5" >/dev/null 2>&1
  n=$(sqlite3 "$tmp" "SELECT count(*) FROM snapshot" 2>/dev/null || echo 0)
  if [[ "$n" =~ ^[0-9]+$ ]] && [ "$n" -gt 0 ]; then echo "ok: -output snapshot"; else echo "FAIL: -output snapshot (got '$n')"; fail=1; fi
else
  echo "skip: -output snapshot (sqlite3 cli not found)"
fi
rm -f "$tmp"

exit $fail

#!/bin/sh
# Bisects the first engine function whose recompiled version breaks the game (PROTORE_RECOMP=all + RANGE).
# A run passes when the game is still running after $RUN_SECONDS without traps. Assumes the failure is caused by a
# single function and that recompiling more functions never fixes it (the smallest failing prefix is found).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
GAME=${PROTORE_GAME:-$HOME/proto-re-data/game}
RUN_SECONDS=${RUN_SECONDS:-70}
export WINEPREFIX=${WINEPREFIX:-$HOME/proto-re-data/wineprefix} WINEDEBUG=-all DXVK_LOG_LEVEL=none
LIST=$(mktemp)
awk -F, 'NR>1 && $3=="false" {print $1}' "$ROOT/re/functions.csv" | sort > "$LIST"
N=$(wc -l < "$LIST")
va_at() { sed -n "$(( $1 + 1 ))p" "$LIST"; }

# run <count>: recompile the first <count> functions; prints pass/fail
run() {
    count=$1
    if [ "$count" -ge "$N" ]; then hi=ffffffff; else hi=$(va_at "$count"); fi
    rm -f "$GAME/proto-re.log"
    (cd "$GAME" && PROTORE_RECOMP=all PROTORE_RECOMP_RANGE="0-$hi" timeout "$RUN_SECONDS" wine prototypef.exe >/dev/null 2>&1)
    code=$?
    wineserver -k 2>/dev/null
    if [ $code -eq 124 ] && ! grep -q "unimplemented" "$GAME/proto-re.log" 2>/dev/null; then echo pass; else echo "fail($code)"; fi
}

lo=0 hi=$N   # invariant: prefix lo passes, prefix hi fails
echo "functions: $N"
r=$(run $hi); echo "all: $r"
case $r in pass) echo "whole engine passes"; exit 0;; esac
while [ $((hi - lo)) -gt 1 ]; do
    mid=$(( (lo + hi) / 2 ))
    r=$(run $mid)
    echo "prefix $mid (< $(va_at $mid)): $r"
    case $r in pass) lo=$mid;; *) hi=$mid;; esac
done
echo "first failing function: index $((hi - 1)) va $(va_at $((hi - 1)))"
rm -f "$LIST"

#!/bin/bash
# Runs every test/<area>_*.script with driver ./<area>test and diffs against the golden.
set -eu
cd "$(dirname "$0")"
pass=0
fail=0
for s in *.script; do
	[ -f "$s" ] || continue
	n=${s%.script}
	drv=./${n%%_*}test
	g=$n.golden
	if [ ! -f "$g" ]; then
		echo "FAIL $n (no golden)"; fail=$((fail+1)); continue
	fi
	if [ ! -x "$drv" ]; then
		echo "FAIL $n (no driver $drv)"; fail=$((fail+1)); continue
	fi
	if $drv < "$s" 2>&1 | diff -u "$g" - ; then
		echo "PASS $n"; pass=$((pass+1))
	else
		echo "FAIL $n"; fail=$((fail+1))
	fi
done
echo "total $((pass+fail)) pass $pass fail $fail"
[ "$fail" -eq 0 ]

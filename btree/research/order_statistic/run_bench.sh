#!/bin/sh
# Runs every variant on every workload, each in a fresh process, appending
# CSV lines to <out>. Run on the M4 with nothing else measuring; see the
# study's §5.5 for the exact sequence.
#
#   sh run_bench.sh <build dir> <out.csv> <rows> <runs> [variants] [workloads]
set -eu
build="$1"
out="$2"
rows="$3"
runs="$4"
variants="${5:-B0 P1 P2 P3 P4 P5 P6}"
workloads="${6:-W1 W2 W3 W4 W5 W6 W7 W8 W9}"
"$build/order_statistic_bench" --header > "$out"
for workload in $workloads; do
    for variant in $variants; do
        run=1
        while [ "$run" -le "$runs" ]; do
            "$build/order_statistic_bench" --variant "$variant" --workload "$workload" \
                --rows "$rows" --run "$run" >> "$out"
            run=$((run + 1))
        done
    done
done
# One checked pass and one counting pass per variant and workload.
for workload in $workloads; do
    for variant in $variants; do
        "$build/order_statistic_bench" --variant "$variant" --workload "$workload" \
            --rows "$rows" --run 0 --check >> "$out.check"
        "$build/order_statistic_bench_counting" --variant "$variant" --workload "$workload" \
            --rows "$rows" --run 0 >> "$out.counting"
    done
done

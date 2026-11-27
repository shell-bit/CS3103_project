#!/usr/bin/env bash
# Run from any directory on Linux with Bash, awk, GCC and GNU timeout.
set -euo pipefail
export LC_ALL=C

usage() { echo "Usage: bash run_tests.sh [--runs N]  (default: 20 runs per case)"; }
runs=20
case "$#:${1-}" in
    0:) ;;
    1:-h|1:--help) usage; exit 0 ;;
    2:--runs) runs=$2 ;;
    1:--runs=*) runs=${1#--runs=} ;;
    *) usage >&2; exit 2 ;;
esac
if [[ $runs =~ ^\+?0*([1-9][0-9]*)$ ]]; then
    runs=${BASH_REMATCH[1]}
else
    echo "--runs must be a positive integer" >&2
    exit 2
fi

test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
source_file="$test_dir/../../problem1.c"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
context=setup
fail() { printf 'FAIL %s: %s\n' "$context" "$*" >&2; exit 1; }

# Configure only the temporary C file. Check every insertion point so a
# source change cannot silently disable a delay or a single-slot trace.
configure_source() {
    awk -v p="$1" -v w="$2" -v c="$3" -v k="$4" -v n="$5" -v m="$6" -v mode="$7" '
        BEGIN {
            value["NUM_PRODUCERS"]=p; value["NUM_WORKERS"]=w; value["NUM_CONSUMERS"]=c
            value["ITEMS_PER_PRODUCER"]=k; value["BUFFER_A_SIZE"]=n; value["BUFFER_B_SIZE"]=m
        }
        { sub(/\r$/, "") }
        $1 == "#define" && ($2 in value) &&
            /^[[:space:]]*#define[[:space:]]+[[:alnum:]_]+[[:space:]]+[0-9]+[[:space:]]*($|\/\/|\/\*)/ {
            definitions[$2]++
            sub(/[0-9]+/, value[$2])
        }
        mode == "worker" && /^[[:space:]]*result_t result = \{/ {
            print "        usleep(2000);"; delays++
        }
        mode == "consumer" && /^[[:space:]]*\} while \(result\.id != STOP_ID\);/ {
            print "        if (result.id != STOP_ID) usleep(2000);"; delays++
        }
        { print }
        mode == "producer" && /^[[:space:]]*item\.value = item\.id \+ 1;/ {
            print "        usleep(2000);"; delays++
        }
        mode == "trace" && /^[[:space:]]*sh->(input|output)\.(in|out)[[:space:]]*=/ {
            buffer = ($0 ~ /sh->input\./ ? "A" : "B")
            action = ($0 ~ /\.in[[:space:]]*=/ ? "+" : "-")
            hits[buffer action]++
            printf "        write(STDERR_FILENO, \"%s%s\\n\", 3);\n", buffer, action
        }
        END {
            for (name in value) {
                if (definitions[name] != 1) {
                    print "Expected one integer definition of " name > "/dev/stderr"; exit 1
                }
            }
            if (mode == "trace" && (hits["A+"] != 2 || hits["A-"] != 1 ||
                                    hits["B+"] != 2 || hits["B-"] != 1)) {
                print "Could not locate all FIFO updates" > "/dev/stderr"; exit 1
            }
            if (mode != "" && mode != "trace" && delays != 1) {
                print "Could not locate the " mode " delay point" > "/dev/stderr"; exit 1
            }
        }
    ' "$source_file" > "$work/problem1.c"
}

# Emit the sequence of worker IDs for one valid run. Scheduling may vary.
check_rows() {
    awk -v expected="$1" -v k="$2" -v workers="$3" '
        function reject(msg) { print msg > "/dev/stderr"; bad=1; exit 1 }
        NF == 0 { next }
        !header {
            if (NF != 4 || $1 != "id" || $2 != "producer_id" || $3 != "worker_id" || $4 != "result")
                reject("Incorrect output header")
            header=1; next
        }
        {
            if (NF != 4) reject("Expected four fields at row " rows+0)
            for (i=1; i<=4; i++)
                if ($i !~ /^[+-]?[0-9]+$/) reject("Expected an integer at row " rows+0)
            if ($1+0 != rows+0 || $2+0 != int(rows/k) || $4+0 != (rows+1)^2)
                reject("Incorrect ID, producer or square: " $0)
            if ($3+0 < 0 || $3+0 >= workers) reject("Invalid worker ID: " $0)
            printf "%d ", $3
            rows++
        }
        END {
            if (bad) exit 1
            if (!header || rows != expected) reject("Expected " expected " rows, got " rows+0)
            print ""
        }
    ' "$work/stdout"
}

check_trace() {
    awk -v a="$1" -v b="$2" '
        {
            buf=substr($0,1,1)
            want=buf ((events[buf] % 2 == 0) ? "+" : "-")
            if ((buf != "A" && buf != "B") || $0 != want) {
                print "Unexpected buffer event at line " NR ": " $0 > "/dev/stderr"; exit 1
            }
            events[buf]++
        }
        END {
            if (events["A"] != 2*a || events["B"] != 2*b) {
                print "Incorrect number of single-slot transfers" > "/dev/stderr"; exit 1
            }
        }
    ' "$work/stderr"
}

total_runs=0
total_rows=0
run_case() {
    local name=$1 p=$2 w=$3 c=$4 k=$5 n=$6 m=$7 mode=${8:-}
    local rows number status distinct
    context=$name
    rows=$((p * k))
    configure_source "$p" "$w" "$c" "$k" "$n" "$m" "$mode" || fail "cannot configure build"
    gcc -std=gnu11 -Wall -Wextra -pthread "$work/problem1.c" -o "$work/problem1" || fail "compilation failed"
    : > "$work/assignments"
    for ((number=1; number<=runs; number++)); do
        context="$name run $number/$runs"
        timeout --kill-after=1s 5s "$work/problem1" </dev/null >"$work/stdout" 2>"$work/stderr" || {
            status=$?
            if ((status == 124)); then fail "exceeded the 5-second timeout"; fi
            head -c 400 "$work/stderr" >&2
            fail "pipeline exited with status $status"
        }
        if [[ $mode == trace ]]; then
            check_trace "$((rows+w))" "$((rows+c))" || fail "invalid single-slot trace"
        elif [[ -s $work/stderr ]]; then
            head -c 400 "$work/stderr" >&2
            fail "unexpected stderr"
        fi
        check_rows "$rows" "$k" "$w" >> "$work/assignments" || fail "incorrect results"
    done
    distinct=$(sort -u "$work/assignments" | wc -l)
    printf 'PASS %s: %d/%d, %d rows/run, %d distinct worker assignments\n' \
        "$name" "$runs" "$runs" "$rows" "$distinct"
    if [[ $mode == trace ]]; then
        printf '  Full/empty pairs per run: A=%d, B=%d\n' "$((rows+w))" "$((rows+c))"
    fi
    total_runs=$((total_runs + runs))
    total_rows=$((total_rows + runs * rows))
}

# The first three configurations are shared with the assignment examples.
{
    read -r _
    while read -r -a fields || ((${#fields[@]})); do
        ((${#fields[@]})) || continue
        ((${#fields[@]} == 7)) || fail "expected a case name and six dimensions"
        for i in 1 2 3 4 5 6; do
            value=${fields[i]%$'\r'}
            [[ $value =~ ^0*([1-9][0-9]*)$ ]] || fail "invalid dimension '$value' in ${fields[0]}"
            fields[i]=${BASH_REMATCH[1]}
        done
        run_case "${fields[@]}"
    done
} < "$test_dir/configurations.txt"
run_case single-slot 4 3 2 30 1 1 trace
for stage in producer worker consumer; do
    run_case "slow-$stage" 2 2 2 24 3 3 "$stage"
done
printf 'PASS: %d runs, %d result rows verified\n' "$total_runs" "$total_rows"

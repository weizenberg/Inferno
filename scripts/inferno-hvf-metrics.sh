#!/usr/bin/env bash
#
# Summarise an Inferno boot into the handful of numbers the HVF work is actually
# judged on, so a change can be evaluated without re-reading a 700 MB trace.
#
#   inferno-hvf-metrics.sh <serial.log> [trace.log]
#   inferno-hvf-metrics.sh -c <A-serial.log> <B-serial.log> [A-trace.log B-trace.log]
#
# The serial metrics need no instrumentation and always work. The trace metrics
# only appear if the build had the matching probes compiled in; they print "-"
# otherwise, so the script stays useful against a stock build.
#
# Why these numbers: a fix here can remove an error message by making the guest
# stop *earlier*, which looks like success if you only grep for the error. Always
# read a positive progress metric (endpoint traffic, boot tasks, exit counts)
# next to the failure strings. See the A/B table in the "give the SEP the whole
# AP shared buffer" commit for a case where that distinction mattered.
#
# Copyright (c) 2026 ChefKiss Inc.
# SPDX-License-Identifier: AGPL-3.0-or-later

set -u

# grep -c already prints 0 when there is no match, and exits non-zero while
# doing so, so do not add an "|| echo 0" fallback -- that emits 0 twice.
count() {
    local n
    n=$(grep -ac -- "$1" "$2" 2>/dev/null)
    printf '%s' "${n:-0}"
}

# Last boot task launchd announced, i.e. how far userspace got.
last_boot_task() {
    grep -aoE 'Doing boot task: [a-z0-9-]+' "$1" 2>/dev/null \
        | tail -1 | sed 's/Doing boot task: //'
}

# Per-endpoint SEP mailbox traffic. Needs the MBOXSEND/MBOXRECV probes in
# hw/misc/apple-silicon/a7iop/mailbox/core.c; prints nothing without them.
endpoint_traffic() {
    [ -n "${1:-}" ] && [ -f "${1:-}" ] || return 0
    grep -aoE 'MBOX(SEND|RECV) [^ ]+ ep=[0-9]+' "$1" 2>/dev/null \
        | grep -oE 'ep=[0-9]+' | sort -t= -k2 -n | uniq -c \
        | awk '{printf "    ep %-4s %s\n", substr($2,4), $1}'
}

trace_metric() {
    # $1 = trace log (may be empty/missing), $2 = pattern
    if [ -z "${1:-}" ] || [ ! -f "${1:-}" ]; then echo "-"; return; fi
    local n
    n=$(grep -ac -- "$2" "$1" 2>/dev/null)
    if [ -z "$n" ] || [ "$n" = 0 ]; then echo "-"; else echo "$n"; fi
}

collect() {
    local serial="$1" trace="${2:-}"

    SERIAL_LINES=$(wc -l < "$serial" | tr -d ' ')
    SEP_ALIVE=$(count 'SEP/OS is alive' "$serial")
    SEP_STAGE1=$(count 'SEP/OS failed to boot at stage 1' "$serial")
    XART_FETCH=$(count 'Fetched SEP-xART' "$serial")
    XART_LOCKER=$(count 'xART Locker' "$serial")
    KS_NEGOTIATE=$(count 'failed to negotiate' "$serial")
    KS_REFKEY=$(count 'failed to create refkey' "$serial")
    KS_DKEY=$(count 'Cannot unwrap d_key' "$serial")
    MP2_FAIL=$(count 'boot task failure: mount-phase-2' "$serial")
    USERPANIC=$(count 'userspace panic' "$serial")
    WDOG=$(count 'wdog panic' "$serial")
    LAUNCHD=$(count 'Doing boot task: launchd' "$serial")
    LAST_TASK=$(last_boot_task "$serial")
    [ -z "$LAST_TASK" ] && LAST_TASK="(none)"

    SEP_EXITS=$(trace_metric "$trace" 'SEPRUN')
    GXF_ENTER=$(trace_metric "$trace" 'GXF enter')
    GXF_EXIT=$(trace_metric "$trace" 'GXF exit')
    EMU_FALLBACK=$(trace_metric "$trace" 'arm_aarch64_fallback_emu_single')
    TRACE_FILE="$trace"
}

report_one() {
    collect "$1" "${2:-}"
    printf '%s\n' "== $1"
    printf '  %-26s %s\n' 'serial lines'            "$SERIAL_LINES"
    printf '  %-26s %s\n' 'last boot task'          "$LAST_TASK"
    printf '  %-26s %s\n' 'reached launchd'         "$LAUNCHD"
    echo '  -- SEP bring-up --'
    printf '  %-26s %s\n' 'SEP/OS is alive'         "$SEP_ALIVE"
    printf '  %-26s %s\n' 'stage-1 panic'           "$SEP_STAGE1"
    printf '  %-26s %s\n' 'SEP-xART fetches'        "$XART_FETCH"
    printf '  %-26s %s\n' 'xART Locker ops'         "$XART_LOCKER"
    echo '  -- keystore --'
    printf '  %-26s %s\n' 'failed to negotiate'     "$KS_NEGOTIATE"
    printf '  %-26s %s\n' 'failed to create refkey' "$KS_REFKEY"
    printf '  %-26s %s\n' 'cannot unwrap d_key'     "$KS_DKEY"
    echo '  -- failure --'
    printf '  %-26s %s\n' 'mount-phase-2 failure'   "$MP2_FAIL"
    printf '  %-26s %s\n' 'userspace panic'         "$USERPANIC"
    printf '  %-26s %s\n' 'wdog panic'              "$WDOG"
    echo '  -- from trace log (needs probes) --'
    printf '  %-26s %s\n' 'SEP vCPU exits'          "$SEP_EXITS"
    printf '  %-26s %s\n' 'GXF enter / exit'        "$GXF_ENTER / $GXF_EXIT"
    printf '  %-26s %s\n' 'MMIO fallback emulated'  "$EMU_FALLBACK"
    if [ -n "${2:-}" ] && [ -f "${2:-}" ]; then
        local ep
        ep=$(endpoint_traffic "$2")
        if [ -n "$ep" ]; then
            echo '  -- SEP mailbox traffic --'
            echo "$ep"
        fi
    fi
}

report_compare() {
    local as="$1" bs="$2" at="${3:-}" bt="${4:-}"
    local -a keys=(
        'serial lines:SERIAL_LINES'
        'last boot task:LAST_TASK'
        'reached launchd:LAUNCHD'
        'SEP/OS is alive:SEP_ALIVE'
        'stage-1 panic:SEP_STAGE1'
        'SEP-xART fetches:XART_FETCH'
        'xART Locker ops:XART_LOCKER'
        'failed to negotiate:KS_NEGOTIATE'
        'failed to create refkey:KS_REFKEY'
        'cannot unwrap d_key:KS_DKEY'
        'mount-phase-2 failure:MP2_FAIL'
        'userspace panic:USERPANIC'
        'wdog panic:WDOG'
        'SEP vCPU exits:SEP_EXITS'
        'MMIO fallback emulated:EMU_FALLBACK'
    )

    collect "$as" "$at"
    local -a avals=()
    local k var
    for k in "${keys[@]}"; do var="${k#*:}"; avals+=("${!var}"); done

    collect "$bs" "$bt"
    printf '%-26s %18s %18s\n' 'metric' 'A' 'B'
    printf '%-26s %18s %18s\n' '------' '-' '-'
    local i=0
    for k in "${keys[@]}"; do
        var="${k#*:}"
        printf '%-26s %18s %18s\n' "${k%%:*}" "${avals[$i]}" "${!var}"
        i=$((i + 1))
    done
    echo
    echo "A = $as   B = $bs"
    echo 'Reminder: a metric going DOWN on the B side is a regression even if a'
    echo 'failure string disappeared -- the guest may simply be stopping sooner.'
}

case "${1:-}" in
-c | --compare)
    shift
    [ $# -ge 2 ] || { echo "usage: $0 -c A-serial B-serial [A-trace B-trace]" >&2; exit 2; }
    report_compare "$1" "$2" "${3:-}" "${4:-}"
    ;;
'' | -h | --help)
    sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
    ;;
*)
    [ -f "$1" ] || { echo "$0: no such serial log: $1" >&2; exit 1; }
    report_one "$1" "${2:-}"
    ;;
esac

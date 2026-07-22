#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
BASE_KEY="${IMR_LSM_TEST_BASE_KEY:-1024}"
MAX_COMPACT_ROUNDS="${IMR_LSM_TEST_COMPACT_ROUNDS:-32}"
BLOCK_SIZE=4096

log()
{
    printf '[imr-lsm tombstone] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm tombstone] FAIL: %s\n' "$*" >&2
    exit 1
}

require_root()
{
    if [[ "${EUID}" -ne 0 ]]; then
        fail "run as root, for example: sudo $0 ${DEVICE}"
    fi
}

require_device()
{
    if [[ ! -b "${DEVICE}" ]]; then
        fail "${DEVICE} is not a block device"
    fi

    case "${DEVICE}" in
        /dev/mapper/imrsim|/dev/mapper/imrsim[0-9]*)
            ;;
        *)
            if [[ "${IMR_LSM_TEST_ALLOW_ANY_DEVICE:-0}" != "1" ]]; then
                fail "refusing to write ${DEVICE}; set IMR_LSM_TEST_ALLOW_ANY_DEVICE=1 to override"
            fi
            ;;
    esac
}

require_debugfs()
{
    local file

    [[ -d "${DEBUGFS}" ]] || fail "missing ${DEBUGFS}; load dm-imrsim and mount debugfs"
    for file in stats block_table delete_key compact compact_segment; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk cmp dd perl mktemp; do
        command -v "${tool}" >/dev/null 2>&1 || fail "missing required tool: ${tool}"
    done
}

stat_value()
{
    local key="$1"

    awk -F': ' -v key="${key}" '$1 == key { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/stats"
}

make_pattern()
{
    local hex="$1"
    local path="$2"

    perl -e 'print chr(hex($ARGV[0])) x $ARGV[1]' "${hex}" "${BLOCK_SIZE}" > "${path}"
}

write_block()
{
    local key="$1"
    local pattern="$2"

    log "write key=${key}"
    dd if="${pattern}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${key}" count=1 \
        conv=notrunc oflag=direct
}

read_block()
{
    local key="$1"
    local output="$2"

    rm -f "${output}"
    dd if="${DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" count=1 \
        iflag=direct 2>"${output}.err"
}

delete_key()
{
    local key="$1"

    log "delete key=${key}"
    printf '%s\n' "${key}" > "${DEBUGFS}/delete_key"
}

compact_active_level()
{
    local reason="$1"
    local level

    level="$(stat_value active_write_level)"
    log "compact L${level}: ${reason}"
    printf '%s\n' "${level}" > "${DEBUGFS}/compact"
}

compact_segment()
{
    printf '1\n' > "${DEBUGFS}/compact_segment"
}

assert_read_equals()
{
    local key="$1"
    local expected="$2"
    local label="$3"
    local output="${TMPDIR}/read-${key}.bin"

    if ! read_block "${key}" "${output}"; then
        fail "${label}: read key=${key} failed, expected exact payload"
    fi
    cmp -s "${expected}" "${output}" || fail "${label}: payload mismatch for key=${key}"
    log "PASS: ${label}"
}

assert_not_patterns()
{
    local key="$1"
    local label="$2"
    shift 2

    local output="${TMPDIR}/read-${key}.bin"
    local pattern

    if ! read_block "${key}" "${output}"; then
        log "PASS: ${label} (read rejected or missed deleted key)"
        return
    fi

    for pattern in "$@"; do
        if cmp -s "${pattern}" "${output}"; then
            fail "${label}: stale payload is still readable for key=${key}"
        fi
    done

    log "PASS: ${label} (read returned no stale payload)"
}

run_segment_compaction_until_idle()
{
    local round
    local score
    local candidates

    for ((round = 1; round <= MAX_COMPACT_ROUNDS; round++)); do
        score="$(stat_value segment_compaction_candidate_score)"
        candidates="$(stat_value segment_compaction_candidate_count)"
        if [[ "${score}" == "0" ]]; then
            log "segment compaction idle after $((round - 1)) round(s)"
            return
        fi

        log "compact_segment round=${round} candidates=${candidates} score=${score}"
        compact_segment
    done

    score="$(stat_value segment_compaction_candidate_score)"
    [[ "${score}" == "0" ]] || fail "segment compaction still has candidate score=${score}"
}

assert_deleted_key_metadata_safe()
{
    local key="$1"
    local label="$2"
    local status

    status="$(
        awk -v key="${key}" '
            $1 ~ /^[0-9]+$/ && $3 == "active" && $5 == key {
                if($14 == 0){
                    tombstone = 1
                    if($9 != 0 || $11 != 0 || $12 != 0 || $13 != 0){
                        bad_tombstone = 1
                    }
                }else{
                    live = 1
                }
            }
            END {
                if(live) exit 3
                if(bad_tombstone) exit 2
                if(tombstone){
                    print "kept"
                }else{
                    print "dropped"
                }
            }
        ' "${DEBUGFS}/block_table"
    )" || fail "${label}: active deleted-key metadata is live or tombstone was copied/mapped"

    case "${status}" in
        kept)
            log "PASS: ${label} (active tombstone is metadata-only)"
            ;;
        dropped)
            log "PASS: ${label} (obsolete tombstone safely dropped)"
            ;;
        *)
            fail "${label}: unexpected metadata status=${status}"
            ;;
    esac
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    local pattern_a="${TMPDIR}/aa.bin"
    local pattern_b="${TMPDIR}/bb.bin"
    local pattern_c="${TMPDIR}/cc.bin"

    make_pattern aa "${pattern_a}"
    make_pattern bb "${pattern_b}"
    make_pattern cc "${pattern_c}"

    local key_delete=$((BASE_KEY + 0))
    local key_delete_rewrite=$((BASE_KEY + 1))
    local key_overwrite_delete=$((BASE_KEY + 2))
    local key_keep=$((BASE_KEY + 3))

    log "device=${DEVICE} debugfs=${DEBUGFS} base_key=${BASE_KEY}"
    log "keys: delete=${key_delete} delete_rewrite=${key_delete_rewrite} overwrite_delete=${key_overwrite_delete} keep=${key_keep}"

    write_block "${key_delete}" "${pattern_a}"
    write_block "${key_delete_rewrite}" "${pattern_a}"
    write_block "${key_overwrite_delete}" "${pattern_a}"
    write_block "${key_keep}" "${pattern_c}"
    compact_active_level "flush initial A/C records into a segment"

    delete_key "${key_delete}"
    delete_key "${key_delete_rewrite}"
    write_block "${key_overwrite_delete}" "${pattern_b}"
    compact_active_level "flush delete/delete/B records into a segment"

    write_block "${key_delete_rewrite}" "${pattern_b}"
    delete_key "${key_overwrite_delete}"
    compact_active_level "flush B/delete records into a segment"

    assert_not_patterns "${key_delete}" \
        "write A -> delete hides old A before segment compaction" \
        "${pattern_a}"
    assert_read_equals "${key_delete_rewrite}" "${pattern_b}" \
        "write A -> delete -> write B reads latest B before segment compaction"
    assert_not_patterns "${key_overwrite_delete}" \
        "write A -> write B -> delete hides old A/B before segment compaction" \
        "${pattern_a}" "${pattern_b}"
    assert_read_equals "${key_keep}" "${pattern_c}" \
        "unrelated live key survives before segment compaction"

    local exec_before
    local copy_before
    local copy_fail_before
    exec_before="$(stat_value segment_compaction_execute_count)"
    copy_before="$(stat_value segment_output_physical_copy_entry_count)"
    copy_fail_before="$(stat_value segment_output_physical_copy_failed_count)"

    run_segment_compaction_until_idle

    local exec_after
    local copy_after
    local copy_fail_after
    exec_after="$(stat_value segment_compaction_execute_count)"
    copy_after="$(stat_value segment_output_physical_copy_entry_count)"
    copy_fail_after="$(stat_value segment_output_physical_copy_failed_count)"

    ((exec_after > exec_before)) || fail "compact_segment did not execute"
    ((copy_after > copy_before)) || fail "physical copy did not copy any valid payload"
    ((copy_fail_after == copy_fail_before)) || fail "physical copy failure counter increased"

    assert_not_patterns "${key_delete}" \
        "write A -> delete still hides old A after segment compaction" \
        "${pattern_a}"
    assert_read_equals "${key_delete_rewrite}" "${pattern_b}" \
        "write A -> delete -> write B still reads latest B after segment compaction"
    assert_not_patterns "${key_overwrite_delete}" \
        "write A -> write B -> delete still hides old A/B after segment compaction" \
        "${pattern_a}" "${pattern_b}"
    assert_read_equals "${key_keep}" "${pattern_c}" \
        "unrelated live key survives after segment compaction"

    assert_deleted_key_metadata_safe "${key_delete}" \
        "deleted key has no copied stale metadata after segment compaction"
    assert_deleted_key_metadata_safe "${key_overwrite_delete}" \
        "overwrite-then-delete key has no copied stale metadata after segment compaction"

    log "PASS: compact executions delta=$((exec_after - exec_before)), physical copy entries delta=$((copy_after - copy_before))"
}

main "$@"

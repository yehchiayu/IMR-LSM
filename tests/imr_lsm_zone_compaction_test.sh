#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_ZONE_COMPACTION_ZONE:-0}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536
TOP_BLOCKS=456
BOTTOM_BLOCKS=568
TRACK_GROUP_BLOCKS=$((TOP_BLOCKS + BOTTOM_BLOCKS))
ZONE_BOTTOM_BLOCKS=$((BOTTOM_BLOCKS * 64))

log()
{
    printf '[imr-lsm zone-compact] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm zone-compact] FAIL: %s\n' "$*" >&2
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
    for file in stats block_table seed_full_zone compact_zone; do
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

    dd if="${pattern}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${key}" count=1 \
        conv=notrunc oflag=direct
}

read_block()
{
    local key="$1"
    local output="$2"

    dd if="${DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" count=1 \
        iflag=direct
}

assert_read_equals()
{
    local key="$1"
    local expected="$2"
    local label="$3"
    local output="${TMPDIR}/read-${key}.bin"

    read_block "${key}" "${output}" ||
        fail "${label}: read key=${key} failed"
    cmp -s "${expected}" "${output}" ||
        fail "${label}: payload mismatch for key=${key}"
    log "PASS: ${label}"
}

latest_active_pba()
{
    local key="$1"

    awk -v key="${key}" '
        $1 ~ /^[0-9]+$/ && $3 == "active" && $5 == key && $14 == 1 {
            if(!found || $15 > ts){
                found = 1
                ts = $15
                pba = $6
            }
        }
        END {
            if(!found) exit 1
            print pba
        }
    ' "${DEBUGFS}/block_table"
}

assert_latest_pba_in_bottom_zone()
{
    local key="$1"
    local expected_zone="$2"
    local label="$3"
    local pba
    local block
    local pba_zone
    local zone_block
    local track_offset

    pba="$(latest_active_pba "${key}")" ||
        fail "${label}: no active valid block-table entry for key=${key}"

    block=$((pba / SECTORS_PER_BLOCK))
    pba_zone=$((block / TOTAL_ITEMS))
    zone_block=$((block % TOTAL_ITEMS))
    track_offset=$((zone_block % TRACK_GROUP_BLOCKS))

    [[ "${pba_zone}" -eq "${expected_zone}" ]] ||
        fail "${label}: expected physical zone ${expected_zone}, got ${pba_zone} (pba=${pba})"
    [[ "${track_offset}" -ge "${TOP_BLOCKS}" ]] ||
        fail "${label}: expected bottom track, got zone_block=${zone_block} pba=${pba}"

    log "PASS: ${label} pba=${pba} zone=${pba_zone} zone_block=${zone_block}"
}

assert_stat_equals()
{
    local key="$1"
    local expected="$2"
    local actual

    actual="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    [[ "${actual}" == "${expected}" ]] ||
        fail "${key}: expected ${expected}, got ${actual}"
    log "PASS: ${key}=${actual}"
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    local zone_start=$((ZONE * TOTAL_ITEMS))
    local key_bottom_first=$((zone_start + 0))
    local key_bottom_last=$((zone_start + ZONE_BOTTOM_BLOCKS - 1))
    local key_top_first=$((zone_start + ZONE_BOTTOM_BLOCKS))
    local key_top_last=$((zone_start + TOTAL_ITEMS - 1))
    local expected_dest_zone1=$((ZONE + 1))
    local before_count
    local after_count

    local pattern_11="${TMPDIR}/11.bin"
    local pattern_22="${TMPDIR}/22.bin"
    local pattern_33="${TMPDIR}/33.bin"
    local pattern_44="${TMPDIR}/44.bin"

    make_pattern 11 "${pattern_11}"
    make_pattern 22 "${pattern_22}"
    make_pattern 33 "${pattern_33}"
    make_pattern 44 "${pattern_44}"

    log "device=${DEVICE} debugfs=${DEBUGFS} source_zone=${ZONE}"
    log "this seeds one full zone metadata: ${TOTAL_ITEMS} blocks (${BLOCK_SIZE} bytes each)"
    log "keys: bottom_first=${key_bottom_first} bottom_last=${key_bottom_last} top_first=${key_top_first} top_last=${key_top_last}"

    log "seed full-zone metadata for VM/debug validation"
    printf '%s\n' "${ZONE}" > "${DEBUGFS}/seed_full_zone"

    log "write non-zero marker blocks"
    write_block "${key_bottom_first}" "${pattern_11}"
    write_block "${key_bottom_last}" "${pattern_22}"
    write_block "${key_top_first}" "${pattern_33}"
    write_block "${key_top_last}" "${pattern_44}"
    sync

    assert_read_equals "${key_bottom_first}" "${pattern_11}" "before compact bottom first marker"
    assert_read_equals "${key_bottom_last}" "${pattern_22}" "before compact bottom last marker"
    assert_read_equals "${key_top_first}" "${pattern_33}" "before compact top first marker"
    assert_read_equals "${key_top_last}" "${pattern_44}" "before compact top last marker"

    before_count="$(stat_value zone_compaction_count || printf '0')"
    log "compact source zone ${ZONE}"
    if ! printf '%s\n' "${ZONE}" > "${DEBUGFS}/compact_zone"; then
        fail "compact_zone failed; recreate a fresh dm target if destination zone ${expected_dest_zone1} is not empty"
    fi
    after_count="$(stat_value zone_compaction_count)"
    [[ "${after_count}" -eq $((before_count + 1)) ]] ||
        fail "zone_compaction_count did not increase by 1: before=${before_count} after=${after_count}"
    log "PASS: zone_compaction_count ${before_count} -> ${after_count}"

    assert_stat_equals last_zone_compaction_source_zone "${ZONE}"
    assert_stat_equals last_zone_compaction_dest_zone0 "${ZONE}"
    assert_stat_equals last_zone_compaction_dest_zone1 "${expected_dest_zone1}"
    assert_stat_equals last_zone_compaction_input_entries "${TOTAL_ITEMS}"
    assert_stat_equals last_zone_compaction_live_entries "${TOTAL_ITEMS}"
    assert_stat_equals last_zone_compaction_skipped_entries 0
    assert_stat_equals last_zone_compaction_failed_entries 0
    assert_stat_equals last_zone_compaction_error 0
    assert_stat_equals last_zone_compaction_copied_entries "$((TOTAL_ITEMS - ZONE_BOTTOM_BLOCKS))"

    assert_read_equals "${key_bottom_first}" "${pattern_11}" "after compact bottom first marker"
    assert_read_equals "${key_bottom_last}" "${pattern_22}" "after compact bottom last marker"
    assert_read_equals "${key_top_first}" "${pattern_33}" "after compact top first marker"
    assert_read_equals "${key_top_last}" "${pattern_44}" "after compact top last marker"

    log "verify block_table/PBA bottom-track placement"
    assert_latest_pba_in_bottom_zone "${key_bottom_first}" "${ZONE}" \
        "key ${key_bottom_first} -> zone ${ZONE} bottom"
    assert_latest_pba_in_bottom_zone "${key_bottom_last}" "${ZONE}" \
        "key ${key_bottom_last} -> zone ${ZONE} bottom"
    assert_latest_pba_in_bottom_zone "${key_top_first}" "${expected_dest_zone1}" \
        "key ${key_top_first} -> zone ${expected_dest_zone1} bottom"
    assert_latest_pba_in_bottom_zone "${key_top_last}" "${expected_dest_zone1}" \
        "key ${key_top_last} -> zone ${expected_dest_zone1} bottom"

    log "PASS: zone-level compaction expanded one full top+bottom zone into two bottom-track regions"
}

main "$@"

#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_AUTO_COMPACTION_ZONE:-2}"
KEY_OFFSET="${IMR_LSM_AUTO_COMPACTION_KEY_OFFSET:-4096}"
WRITE_COUNT="${IMR_LSM_AUTO_COMPACTION_WRITES:-20}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536
COMPACTION_THRESHOLD=16

log()
{
    printf '[imr-lsm auto-compact] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm auto-compact] FAIL: %s\n' "$*" >&2
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

    [[ -d "${DEBUGFS}" ]] ||
        fail "missing ${DEBUGFS}; load dm-imrsim and mount debugfs"
    for file in stats segments block_table clear_read_tree; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_range()
{
    local sectors
    local zone_count

    [[ "${WRITE_COUNT}" -gt "${COMPACTION_THRESHOLD}" ]] ||
        fail "WRITE_COUNT=${WRITE_COUNT} must be > ${COMPACTION_THRESHOLD}"
    [[ "${KEY_OFFSET}" -ge 0 ]] ||
        fail "KEY_OFFSET=${KEY_OFFSET} must be >= 0"
    [[ $((KEY_OFFSET + WRITE_COUNT)) -le "${TOTAL_ITEMS}" ]] ||
        fail "key range crosses zone: offset=${KEY_OFFSET} count=${WRITE_COUNT}"

    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    zone_count=$((sectors / SECTORS_PER_BLOCK / TOTAL_ITEMS))
    [[ "${ZONE}" -lt "${zone_count}" ]] ||
        fail "zone ${ZONE} is outside ${DEVICE}; available zones=${zone_count}"
}

stat_value()
{
    local key="$1"

    awk -F': ' -v key="${key}" '$1 == key { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/stats"
}

stat_number()
{
    local key="$1"
    local value

    value="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    case "${value}" in
        ''|*[!0-9]*)
            fail "stat ${key} is not numeric: ${value}"
            ;;
    esac
    printf '%s\n' "${value}"
}

segment_count()
{
    awk -F': ' '$1 == "segment_count" { print $2; found = 1; exit }
        END { if(!found) exit 1 }' "${DEBUGFS}/segments" ||
        fail "missing segment_count"
}

block_table_entries_in_range()
{
    local first_key="$1"
    local count="$2"

    awk -v first="${first_key}" -v count="${count}" '
        $1 ~ /^[0-9]+$/ && $5 >= first && $5 < first + count { hits++ }
        END { print hits + 0 }
    ' "${DEBUGFS}/block_table"
}

assert_counter_delta_ge()
{
    local key="$1"
    local before="$2"
    local minimum="$3"
    local label="$4"
    local after
    local delta

    after="$(stat_number "${key}")"
    delta=$((after - before))
    [[ "${delta}" -ge "${minimum}" ]] ||
        fail "${label}: ${key} delta expected >= ${minimum}, got ${delta} (before=${before} after=${after})"
    log "PASS: ${label}: ${key} +${delta} >= ${minimum}"
}

assert_counter_delta_eq()
{
    local key="$1"
    local before="$2"
    local expected="$3"
    local label="$4"
    local after
    local delta

    after="$(stat_number "${key}")"
    delta=$((after - before))
    [[ "${delta}" -eq "${expected}" ]] ||
        fail "${label}: ${key} delta expected ${expected}, got ${delta} (before=${before} after=${after})"
    log "PASS: ${label}: ${key} +${delta}"
}

make_pattern()
{
    local index="$1"
    local path="$2"

    perl -e 'print chr(($ARGV[0] % 251) + 1) x $ARGV[1]' \
        "${index}" "${BLOCK_SIZE}" > "${path}"
}

write_block()
{
    local key="$1"
    local pattern="$2"

    dd if="${pattern}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${key}" \
        count=1 conv=notrunc oflag=direct status=none
}

read_block()
{
    local key="$1"
    local output="$2"

    rm -f "${output}"
    dd if="${DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" \
        count=1 iflag=direct status=none
}

clear_read_tree()
{
    printf '1\n' > "${DEBUGFS}/clear_read_tree"
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools
    require_range

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    local first_key=$((ZONE * TOTAL_ITEMS + KEY_OFFSET))
    local before_compaction
    local before_record_insert
    local before_fallback
    local before_segment_count
    local after_segment_count
    local block_table_hits
    local i

    log "device=${DEVICE} debugfs=${DEBUGFS} zone=${ZONE} first_key=${first_key} writes=${WRITE_COUNT}"
    log "writing ${WRITE_COUNT} normal 4K inserts; threshold=${COMPACTION_THRESHOLD}"

    before_compaction="$(stat_number compaction_count)"
    before_record_insert="$(stat_number lsm_record_insert_count)"
    before_segment_count="$(segment_count)"

    for ((i = 0; i < WRITE_COUNT; i++)); do
        local key=$((first_key + i))
        local pattern="${TMPDIR}/pattern-${i}.bin"

        make_pattern "${i}" "${pattern}"
        write_block "${key}" "${pattern}"
    done
    sync

    assert_counter_delta_eq lsm_record_insert_count "${before_record_insert}" \
        "${WRITE_COUNT}" "normal insert records"
    assert_counter_delta_ge compaction_count "${before_compaction}" 1 \
        "automatic threshold compaction"

    after_segment_count="$(segment_count)"
    [[ "${after_segment_count}" -gt "${before_segment_count}" ]] ||
        fail "segment_count did not increase: before=${before_segment_count} after=${after_segment_count}"
    log "PASS: segment_count ${before_segment_count} -> ${after_segment_count}"

    block_table_hits="$(block_table_entries_in_range "${first_key}" "${WRITE_COUNT}")"
    [[ "${block_table_hits}" -gt 0 ]] ||
        fail "block_table has no entries for keys ${first_key}..$((first_key + WRITE_COUNT - 1))"
    log "PASS: block_table entries in test range=${block_table_hits}"

    clear_read_tree
    before_fallback="$(stat_number fallback_count)"
    for ((i = 0; i < WRITE_COUNT; i++)); do
        local key=$((first_key + i))
        local expected="${TMPDIR}/pattern-${i}.bin"
        local output="${TMPDIR}/read-${i}.bin"

        read_block "${key}" "${output}"
        cmp -s "${expected}" "${output}" ||
            fail "payload mismatch for key=${key}"
    done
    assert_counter_delta_eq fallback_count "${before_fallback}" 0 \
        "read back uses LSM mappings"

    log "PASS: normal inserts auto-compacted into segment/block_table and all payloads read back correctly"
}

main "$@"

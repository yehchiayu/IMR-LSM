#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_DISCARD_ZONE:-3}"
KEY_OFFSET="${IMR_LSM_DISCARD_KEY_OFFSET:-4096}"
DELETE_BLOCKS="${IMR_LSM_DISCARD_BLOCKS:-3}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536

log()
{
    printf '[imr-lsm discard] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm discard] FAIL: %s\n' "$*" >&2
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
    for file in stats; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk blkdiscard blockdev cmp dd perl mktemp sync; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_range()
{
    local sectors
    local zone_count

    [[ "${DELETE_BLOCKS}" -gt 0 ]] ||
        fail "DELETE_BLOCKS=${DELETE_BLOCKS} must be > 0"
    [[ "${KEY_OFFSET}" -ge 0 ]] ||
        fail "KEY_OFFSET=${KEY_OFFSET} must be >= 0"
    [[ $((KEY_OFFSET + DELETE_BLOCKS + 1)) -le "${TOTAL_ITEMS}" ]] ||
        fail "test range crosses zone: offset=${KEY_OFFSET} blocks=${DELETE_BLOCKS}"

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

have_stat()
{
    local key="$1"

    stat_value "${key}" >/dev/null 2>&1
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

make_pattern()
{
    local hex="$1"
    local path="$2"

    perl -e 'print chr(hex($ARGV[0])) x $ARGV[1]' \
        "${hex}" "${BLOCK_SIZE}" > "${path}"
}

write_block()
{
    local key="$1"
    local pattern="$2"

    log "write key=${key}"
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

discard_blocks()
{
    local first_key="$1"
    local blocks="$2"
    local offset=$((first_key * BLOCK_SIZE))
    local length=$((blocks * BLOCK_SIZE))

    log "discard first_key=${first_key} blocks=${blocks} offset=${offset} length=${length}"
    blkdiscard -o "${offset}" -l "${length}" "${DEVICE}"
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
    local live_key=$((first_key + DELETE_BLOCKS))
    local before_delete
    local before_discard_bio
    local before_discard_delete
    local have_discard_counters=0
    local before_tombstone
    local i

    log "device=${DEVICE} debugfs=${DEBUGFS} zone=${ZONE} first_key=${first_key} blocks=${DELETE_BLOCKS}"

    for ((i = 0; i < DELETE_BLOCKS; i++)); do
        local pattern="${TMPDIR}/delete-${i}.bin"

        make_pattern "$((0x30 + i))" "${pattern}"
        write_block "$((first_key + i))" "${pattern}"
        assert_read_equals "$((first_key + i))" "${pattern}" \
            "pre-discard key=$((first_key + i)) reads written payload"
    done

    local live_pattern="${TMPDIR}/live.bin"
    make_pattern 7A "${live_pattern}"
    write_block "${live_key}" "${live_pattern}"
    assert_read_equals "${live_key}" "${live_pattern}" \
        "pre-discard neighbor key=${live_key} reads written payload"

    before_delete="$(stat_number delete_count)"
    if have_stat discard_bio_count && have_stat discard_delete_count; then
        have_discard_counters=1
        before_discard_bio="$(stat_number discard_bio_count)"
        before_discard_delete="$(stat_number discard_delete_count)"
    else
        log "SKIP: discard_bio_count/discard_delete_count not present; using delete_count/tombstone evidence"
    fi
    before_tombstone="$(stat_number tombstone_hit_count)"
    discard_blocks "${first_key}" "${DELETE_BLOCKS}"
    sync

    if [[ "${have_discard_counters}" -eq 1 ]]; then
        assert_counter_delta_eq discard_bio_count "${before_discard_bio}" 1 \
            "blkdiscard reaches dm target"
        assert_counter_delta_eq discard_delete_count "${before_discard_delete}" 1 \
            "blkdiscard completes metadata range delete"
    fi
    assert_counter_delta_eq delete_count "${before_delete}" "${DELETE_BLOCKS}" \
        "range discard records tombstones"

    for ((i = 0; i < DELETE_BLOCKS; i++)); do
        assert_not_patterns "$((first_key + i))" \
            "discarded key=$((first_key + i)) hides old payload" \
            "${TMPDIR}/delete-${i}.bin"
    done
    assert_counter_delta_ge tombstone_hit_count "${before_tombstone}" 1 \
        "discarded reads observe tombstone path"
    assert_read_equals "${live_key}" "${live_pattern}" \
        "neighbor key=${live_key} survives discard range"

    local rewrite_pattern="${TMPDIR}/rewrite.bin"
    make_pattern 55 "${rewrite_pattern}"
    write_block "$((first_key + 1))" "${rewrite_pattern}"
    assert_read_equals "$((first_key + 1))" "${rewrite_pattern}" \
        "rewrite after discard is readable"

    before_delete="$(stat_number delete_count)"
    discard_blocks "$((first_key + 1))" 1
    sync
    assert_counter_delta_eq delete_count "${before_delete}" 1 \
        "single-block discard after rewrite"
    assert_not_patterns "$((first_key + 1))" \
        "single-block discard hides rewritten payload" \
        "${rewrite_pattern}"

    log "PASS: discard/TRIM range delete hides stale payloads and preserves out-of-range data"
}

main "$@"

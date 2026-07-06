#!/usr/bin/env bash
set -euo pipefail

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_READ_PATH_ZONE:-1}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536

log()
{
    printf '[imr-lsm read-path] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm read-path] FAIL: %s\n' "$*" >&2
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
    for file in stats read_tree clear_read_tree delete_key compact seed_full_zone zone_compaction_auto_run; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp; do
        command -v "${tool}" >/dev/null 2>&1 || fail "missing required tool: ${tool}"
    done
}

require_zone_exists()
{
    local sectors
    local zone_count

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

assert_stat_equals()
{
    local key="$1"
    local expected="$2"
    local actual

    actual="$(stat_number "${key}")"
    [[ "${actual}" == "${expected}" ]] ||
        fail "${key}: expected ${expected}, got ${actual}"
    log "PASS: ${key}=${actual}"
}

assert_stat_ge()
{
    local key="$1"
    local minimum="$2"
    local actual

    actual="$(stat_number "${key}")"
    [[ "${actual}" -ge "${minimum}" ]] ||
        fail "${key}: expected >= ${minimum}, got ${actual}"
    log "PASS: ${key}=${actual} >= ${minimum}"
}

assert_counter_delta()
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

clear_read_tree()
{
    printf '1\n' > "${DEBUGFS}/clear_read_tree"
    assert_stat_equals read_tree_size 0
}

compact_active_level()
{
    local reason="$1"
    local level

    level="$(stat_number active_write_level)"
    log "compact L${level}: ${reason}"
    printf '%s\n' "${level}" > "${DEBUGFS}/compact"
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

disable_zone_compaction_auto_run()
{
    printf '0\n' > "${DEBUGFS}/zone_compaction_auto_run"
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools
    require_zone_exists

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf "${TMPDIR}"' EXIT

    local zone_start=$((ZONE * TOTAL_ITEMS))
    local key_tree=$((zone_start + 16))
    local key_unsorted=$((zone_start + 17))
    local key_segment=$((zone_start + 18))
    local key_delete=$((zone_start + 19))
    local key_fallback=$((zone_start + 2048))

    local pattern_tree="${TMPDIR}/11.bin"
    local pattern_unsorted="${TMPDIR}/22.bin"
    local pattern_segment="${TMPDIR}/33.bin"
    local pattern_delete="${TMPDIR}/44.bin"
    local fallback_output="${TMPDIR}/fallback.bin"

    make_pattern 11 "${pattern_tree}"
    make_pattern 22 "${pattern_unsorted}"
    make_pattern 33 "${pattern_segment}"
    make_pattern 44 "${pattern_delete}"

    log "device=${DEVICE} debugfs=${DEBUGFS} zone=${ZONE}"
    log "keys: tree=${key_tree} unsorted=${key_unsorted} segment=${key_segment} delete=${key_delete} fallback=${key_fallback}"

    local before_tree_hit
    local before_read_tree_hit
    local before_unsorted

    write_block "${key_tree}" "${pattern_tree}"
    sync
    before_tree_hit="$(stat_number tree_hit_count)"
    before_read_tree_hit="$(stat_number read_tree_hit_count)"
    before_unsorted="$(stat_number unsorted_hit_count)"
    assert_read_equals "${key_tree}" "${pattern_tree}" \
        "tree index hit returns the latest write"
    assert_counter_delta tree_hit_count "${before_tree_hit}" 1 \
        "tree hit short-circuits read path"
    assert_counter_delta read_tree_hit_count "${before_read_tree_hit}" 1 \
        "read tree lookup hit"
    assert_counter_delta unsorted_hit_count "${before_unsorted}" 0 \
        "tree hit does not fall through to unsorted"
    assert_stat_equals last_read_tree_key "${key_tree}"
    assert_stat_equals last_read_tree_hit 1
    assert_stat_equals last_read_tree_valid 1

    local before_tree_miss
    local before_segment
    local before_fallback

    write_block "${key_unsorted}" "${pattern_unsorted}"
    sync
    clear_read_tree
    before_tree_miss="$(stat_number read_tree_miss_count)"
    before_unsorted="$(stat_number unsorted_hit_count)"
    before_segment="$(stat_number segment_hit_count)"
    before_fallback="$(stat_number fallback_count)"
    assert_read_equals "${key_unsorted}" "${pattern_unsorted}" \
        "tree miss falls through to unsorted list"
    assert_counter_delta read_tree_miss_count "${before_tree_miss}" 1 \
        "unsorted case tree miss"
    assert_counter_delta unsorted_hit_count "${before_unsorted}" 1 \
        "unsorted list hit"
    assert_counter_delta segment_hit_count "${before_segment}" 0 \
        "unsorted latest record wins before segment"
    assert_counter_delta fallback_count "${before_fallback}" 0 \
        "unsorted hit avoids disk fallback"
    assert_stat_equals last_read_tree_key "${key_unsorted}"
    assert_stat_equals last_read_tree_hit 0

    local before_block_table_hit
    local before_sorted

    write_block "${key_segment}" "${pattern_segment}"
    sync
    compact_active_level "flush records into segment block_table"
    clear_read_tree
    before_tree_miss="$(stat_number read_tree_miss_count)"
    before_unsorted="$(stat_number unsorted_hit_count)"
    before_segment="$(stat_number segment_hit_count)"
    before_sorted="$(stat_number sorted_hit_count)"
    before_block_table_hit="$(stat_number block_table_hit_count)"
    before_fallback="$(stat_number fallback_count)"
    assert_read_equals "${key_segment}" "${pattern_segment}" \
        "tree miss falls through to segment bloom/block_table"
    assert_counter_delta read_tree_miss_count "${before_tree_miss}" 1 \
        "segment case tree miss"
    assert_counter_delta unsorted_hit_count "${before_unsorted}" 0 \
        "segment case skips unsorted source"
    assert_counter_delta segment_hit_count "${before_segment}" 1 \
        "segment block_table hit"
    assert_counter_delta sorted_hit_count "${before_sorted}" 0 \
        "segment hit wins before sorted fallback"
    assert_counter_delta_ge block_table_hit_count "${before_block_table_hit}" 1 \
        "block_table confirms segment hit"
    assert_counter_delta fallback_count "${before_fallback}" 0 \
        "segment hit avoids disk fallback"
    assert_stat_equals last_read_tree_key "${key_segment}"
    assert_stat_equals last_read_tree_hit 0
    assert_stat_equals last_block_table_hit_key "${key_segment}"
    assert_stat_equals last_block_table_hit_valid 1
    assert_stat_ge last_segment_lookup_count 1
    assert_stat_ge last_bloom_lookup_count 1
    assert_stat_ge last_block_table_lookup_count 1

    local before_tombstone

    write_block "${key_delete}" "${pattern_delete}"
    sync
    compact_active_level "place stale delete payload into segment"
    delete_key "${key_delete}"
    before_read_tree_hit="$(stat_number read_tree_hit_count)"
    before_tombstone="$(stat_number tombstone_hit_count)"
    before_fallback="$(stat_number fallback_count)"
    assert_not_patterns "${key_delete}" \
        "tree tombstone hides older segment payload" \
        "${pattern_delete}"
    assert_counter_delta read_tree_hit_count "${before_read_tree_hit}" 1 \
        "tombstone is served by read tree"
    assert_counter_delta tombstone_hit_count "${before_tombstone}" 1 \
        "tree tombstone counted"
    assert_counter_delta fallback_count "${before_fallback}" 0 \
        "tombstone avoids disk fallback"
    assert_stat_equals last_read_tree_key "${key_delete}"
    assert_stat_equals last_read_tree_hit 1
    assert_stat_equals last_read_tree_valid 0

    local before_read_miss

    disable_zone_compaction_auto_run
    log "seed zone ${ZONE} metadata for disk fallback validation"
    printf '%s\n' "${ZONE}" > "${DEBUGFS}/seed_full_zone"
    clear_read_tree
    before_tree_miss="$(stat_number read_tree_miss_count)"
    before_unsorted="$(stat_number unsorted_hit_count)"
    before_segment="$(stat_number segment_hit_count)"
    before_tombstone="$(stat_number tombstone_hit_count)"
    before_read_miss="$(stat_number read_miss_count)"
    before_fallback="$(stat_number fallback_count)"
    read_block "${key_fallback}" "${fallback_output}" ||
        fail "disk fallback read key=${key_fallback} failed"
    assert_counter_delta read_tree_miss_count "${before_tree_miss}" 1 \
        "fallback case tree miss"
    assert_counter_delta unsorted_hit_count "${before_unsorted}" 0 \
        "fallback has no unsorted LSM hit"
    assert_counter_delta segment_hit_count "${before_segment}" 0 \
        "fallback has no segment LSM hit"
    assert_counter_delta tombstone_hit_count "${before_tombstone}" 0 \
        "fallback is not a tombstone"
    assert_counter_delta read_miss_count "${before_read_miss}" 1 \
        "LSM miss before disk fallback"
    assert_counter_delta fallback_count "${before_fallback}" 1 \
        "disk fallback path reached"
    assert_stat_equals last_read_tree_key "${key_fallback}"
    assert_stat_equals last_read_tree_hit 0

    log "PASS: read path order validated: tree -> unsorted -> segment/bloom/block_table -> tombstone masking -> disk fallback"
}

main "$@"

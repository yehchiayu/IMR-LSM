#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_ZONE_TOMBSTONE_ZONE:-2}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536
TOP_BLOCKS=456
BOTTOM_BLOCKS=568
TRACK_GROUP_BLOCKS=$((TOP_BLOCKS + BOTTOM_BLOCKS))
ZONE_BOTTOM_BLOCKS=$((BOTTOM_BLOCKS * 64))
TMPDIR=""
ORIGINAL_ZONE_COMPACTION_AUTO_RUN=""

log()
{
    printf '[imr-lsm zone-tombstone] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm zone-tombstone] FAIL: %s\n' "$*" >&2
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
    for file in stats block_table delete_key seed_full_zone compact_zone \
        zone_compaction_auto_run; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

cleanup()
{
    set +e
    if [[ -n "${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}" ]]; then
        printf '%s\n' "${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}" \
            > "${DEBUGFS}/zone_compaction_auto_run"
    fi
    if [[ -n "${TMPDIR}" && -d "${TMPDIR}" ]]; then
        rm -rf -- "${TMPDIR}"
    fi
}

disable_auto_zone_compaction()
{
    ORIGINAL_ZONE_COMPACTION_AUTO_RUN="$(
        tr -d '[:space:]' < "${DEBUGFS}/zone_compaction_auto_run"
    )" || fail "cannot read zone_compaction_auto_run"
    [[ "${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}" == "0" ||
       "${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}" == "1" ]] ||
        fail "invalid zone_compaction_auto_run value: ${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}"
    printf '0\n' > "${DEBUGFS}/zone_compaction_auto_run"
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp; do
        command -v "${tool}" >/dev/null 2>&1 || fail "missing required tool: ${tool}"
    done
}

require_zone_range()
{
    local sectors
    local zone_count

    imr_lsm_test_require_nonnegative_integer ZONE "${ZONE}"
    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    zone_count=$((sectors / SECTORS_PER_BLOCK / TOTAL_ITEMS))
    [[ "${ZONE}" -lt "${zone_count}" ]] ||
        fail "source zone ${ZONE} is outside ${DEVICE}; available zones=${zone_count}"
    [[ "${zone_count}" -ge 3 ]] ||
        fail "test needs source, active-append, and GC destination zones; available zones=${zone_count}"
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

    log "write marker key=${key}"
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

max_active_segment_id()
{
    awk '
        $1 ~ /^[0-9]+$/ && $3 == "active" {
            if(!found || $1 > max_id){
                found = 1
                max_id = $1
            }
        }
        END { print found ? max_id : 0 }
    ' "${DEBUGFS}/block_table"
}

assert_no_new_active_valid_block_table_entry()
{
    local key="$1"
    local previous_max_segment_id="$2"
    local label="$3"

    if awk -v key="${key}" -v previous_max="${previous_max_segment_id}" '
        $1 ~ /^[0-9]+$/ && $1 > previous_max &&
        $3 == "active" && $5 == key && $14 == 1 {
            found = 1
        }
        END { exit found ? 0 : 1 }
    ' "${DEBUGFS}/block_table"; then
        fail "${label}: active valid block-table entry still exists"
    fi

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
    imr_lsm_test_safety_begin "${DEVICE}"
    require_zone_range

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT
    disable_auto_zone_compaction

    local zone_start=$((ZONE * TOTAL_ITEMS))
    local key_live_bottom=$((zone_start + 0))
    local key_live_top=$((zone_start + ZONE_BOTTOM_BLOCKS))
    local key_delete_top=$((zone_start + TOTAL_ITEMS - 1))
    local key_gc_probe=$((zone_start + 1))
    local marker_pba
    local marker_zone
    local expected_dest_zone
    local before_count
    local after_count
    local before_max_segment_id

    local pattern_a="${TMPDIR}/aa.bin"
    local pattern_b="${TMPDIR}/bb.bin"
    local pattern_c="${TMPDIR}/cc.bin"

    make_pattern aa "${pattern_a}"
    make_pattern bb "${pattern_b}"
    make_pattern cc "${pattern_c}"

    log "device=${DEVICE} debugfs=${DEBUGFS} source_zone=${ZONE}"
    log "keys: live_bottom=${key_live_bottom} live_top=${key_live_top} delete_top=${key_delete_top}"

    log "seed full-zone metadata for VM/debug validation"
    printf '%s\n' "${ZONE}" > "${DEBUGFS}/seed_full_zone"

    write_block "${key_delete_top}" "${pattern_a}"
    write_block "${key_live_bottom}" "${pattern_b}"
    write_block "${key_live_top}" "${pattern_c}"
    sync

    delete_key "${key_delete_top}"

    assert_not_patterns "${key_delete_top}" \
        "deleted top key hides old payload before zone compaction" \
        "${pattern_a}"
    assert_read_equals "${key_live_bottom}" "${pattern_b}" \
        "live bottom key reads before zone compaction"
    assert_read_equals "${key_live_top}" "${pattern_c}" \
        "live top key reads before zone compaction"

    marker_pba="$(latest_active_pba "${key_live_bottom}")" ||
        fail "cannot resolve active append zone for key=${key_live_bottom}"
    marker_zone=$((marker_pba / SECTORS_PER_BLOCK / TOTAL_ITEMS))
    [[ "${marker_zone}" -ne "${ZONE}" ]] ||
        fail "logical overwrite was not appended outside sealed source zone ${ZONE}"

    before_count="$(stat_value zone_compaction_count || printf '0')"
    before_max_segment_id="$(max_active_segment_id)"
    log "compact source zone ${ZONE}"
    if ! printf '%s\n' "${ZONE}" > "${DEBUGFS}/compact_zone"; then
        fail "compact_zone failed; recreate a fresh dm target with an empty free-pool destination"
    fi
    after_count="$(stat_value zone_compaction_count)"
    expected_dest_zone="$(stat_value last_zone_compaction_dest_zone0)" ||
        fail "missing last_zone_compaction_dest_zone0"
    [[ "${expected_dest_zone}" =~ ^[0-9]+$ &&
       "${expected_dest_zone}" -ne "${ZONE}" &&
       "${expected_dest_zone}" -ne "${marker_zone}" ]] ||
        fail "invalid free-pool destination: ${expected_dest_zone}"
    [[ "${after_count}" -eq $((before_count + 1)) ]] ||
        fail "zone_compaction_count did not increase by 1: before=${before_count} after=${after_count}"
    log "PASS: zone_compaction_count ${before_count} -> ${after_count}"

    assert_stat_equals last_zone_compaction_source_zone "${ZONE}"
    assert_stat_equals last_zone_compaction_dest_zone0 "${expected_dest_zone}"
    assert_stat_equals last_zone_compaction_dest_zone1 none
    assert_stat_equals last_zone_compaction_input_entries "${TOTAL_ITEMS}"
    assert_stat_equals last_zone_compaction_live_entries "$((TOTAL_ITEMS - 3))"
    assert_stat_equals last_zone_compaction_skipped_entries 3
    assert_stat_equals last_zone_compaction_failed_entries 0
    assert_stat_equals last_zone_compaction_error 0
    assert_stat_equals last_zone_compaction_copied_entries "$((TOTAL_ITEMS - 3))"

    assert_not_patterns "${key_delete_top}" \
        "deleted top key hides old payload after zone compaction" \
        "${pattern_a}"
    assert_read_equals "${key_live_bottom}" "${pattern_b}" \
        "live bottom key survives zone compaction"
    assert_read_equals "${key_live_top}" "${pattern_c}" \
        "live top key survives zone compaction"

    assert_no_new_active_valid_block_table_entry \
        "${key_delete_top}" "${before_max_segment_id}" \
        "deleted top key is not moved into the new compacted block_table"
    assert_latest_pba_in_bottom_zone "${key_live_bottom}" "${marker_zone}" \
        "live bottom key remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_live_top}" "${marker_zone}" \
        "live top key remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_gc_probe}" "${expected_dest_zone}" \
        "untouched source key moves to free-pool zone ${expected_dest_zone}"

    log "PASS: zone GC skips invalid records, preserves live data, hides tombstones, and reclaims the victim"
}

main "$@"

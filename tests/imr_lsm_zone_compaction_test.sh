#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_ZONE_COMPACTION_ZONE:-0}"
COMPACTION_MODE="${IMR_LSM_ZONE_COMPACTION_MODE:-manual}"
AUTO_TIMEOUT="${IMR_LSM_ZONE_COMPACTION_AUTO_TIMEOUT:-120}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
TOTAL_ITEMS=65536
TOP_BLOCKS=456
BOTTOM_BLOCKS=568
TRACK_GROUP_BLOCKS=$((TOP_BLOCKS + BOTTOM_BLOCKS))
ZONE_BOTTOM_BLOCKS=$((BOTTOM_BLOCKS * 64))
TMPDIR=""
ORIGINAL_ZONE_COMPACTION_AUTO_RUN=""
ORIGINAL_ZONE_GC_MIN_INVALID_RATIO=""

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
    for file in stats block_table seed_full_zone compact_zone \
        zone_compaction_auto_run zone_gc_min_invalid_ratio_permille; do
        [[ -e "${DEBUGFS}/${file}" ]] || fail "missing ${DEBUGFS}/${file}"
    done
}

cleanup()
{
    set +e
    if [[ -n "${ORIGINAL_ZONE_COMPACTION_AUTO_RUN}" ]]; then
        printf '0\n' > "${DEBUGFS}/zone_compaction_auto_run"
    fi
    if [[ -n "${ORIGINAL_ZONE_GC_MIN_INVALID_RATIO}" ]]; then
        printf '%s\n' "${ORIGINAL_ZONE_GC_MIN_INVALID_RATIO}" \
            > "${DEBUGFS}/zone_gc_min_invalid_ratio_permille"
    fi
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
    ORIGINAL_ZONE_GC_MIN_INVALID_RATIO="$(
        tr -d '[:space:]' \
            < "${DEBUGFS}/zone_gc_min_invalid_ratio_permille"
    )" || fail "cannot read zone_gc_min_invalid_ratio_permille"
    imr_lsm_test_require_nonnegative_integer \
        zone_gc_min_invalid_ratio_permille \
        "${ORIGINAL_ZONE_GC_MIN_INVALID_RATIO}"
    printf '0\n' > "${DEBUGFS}/zone_compaction_auto_run"
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd perl mktemp sleep; do
        command -v "${tool}" >/dev/null 2>&1 || fail "missing required tool: ${tool}"
    done
}

validate_mode()
{
    case "${COMPACTION_MODE}" in
        manual|auto)
            ;;
        *)
            fail "IMR_LSM_ZONE_COMPACTION_MODE must be manual or auto"
            ;;
    esac
    imr_lsm_test_require_nonnegative_integer AUTO_TIMEOUT "${AUTO_TIMEOUT}"
    [[ "${AUTO_TIMEOUT}" -gt 0 ]] || fail "AUTO_TIMEOUT must be greater than zero"
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
    validate_mode
    imr_lsm_test_safety_begin "${DEVICE}"
    require_zone_range

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT
    disable_auto_zone_compaction

    local zone_start=$((ZONE * TOTAL_ITEMS))
    local key_bottom_first=$((zone_start + 0))
    local key_bottom_last=$((zone_start + ZONE_BOTTOM_BLOCKS - 1))
    local key_top_first=$((zone_start + ZONE_BOTTOM_BLOCKS))
    local key_top_last=$((zone_start + TOTAL_ITEMS - 1))
    local key_gc_probe=$((zone_start + 1))
    local marker_pba
    local marker_zone
    local expected_dest_zone
    local before_count
    local after_count
    local before_auto_run_count
    local before_auto_failed_count
    local before_zone_failed_count

    local pattern_11="${TMPDIR}/11.bin"
    local pattern_22="${TMPDIR}/22.bin"
    local pattern_33="${TMPDIR}/33.bin"
    local pattern_44="${TMPDIR}/44.bin"

    make_pattern 11 "${pattern_11}"
    make_pattern 22 "${pattern_22}"
    make_pattern 33 "${pattern_33}"
    make_pattern 44 "${pattern_44}"

    log "device=${DEVICE} debugfs=${DEBUGFS} source_zone=${ZONE} mode=${COMPACTION_MODE}"
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

    marker_pba="$(latest_active_pba "${key_bottom_first}")" ||
        fail "cannot resolve active append zone for marker key=${key_bottom_first}"
    marker_zone=$((marker_pba / SECTORS_PER_BLOCK / TOTAL_ITEMS))
    [[ "${marker_zone}" -ne "${ZONE}" ]] ||
        fail "logical overwrite was not appended outside sealed source zone ${ZONE}"

    before_count="$(stat_value zone_compaction_count || printf '0')"
    if [[ "${COMPACTION_MODE}" == "auto" ]]; then
        local deadline=$((SECONDS + AUTO_TIMEOUT))
        local after_auto_run_count
        local after_auto_failed_count
        local after_zone_failed_count
        local last_auto_zone
        local pending
        local running
        local last_error

        before_auto_run_count="$(stat_value zone_compaction_auto_run_count)"
        before_auto_failed_count="$(stat_value zone_compaction_auto_run_failed_count)"
        before_zone_failed_count="$(stat_value zone_compaction_failed_count)"
        log "set zone GC invalid-ratio threshold to 0 for seeded auto-path validation"
        printf '0\n' \
            > "${DEBUGFS}/zone_gc_min_invalid_ratio_permille"
        assert_stat_equals zone_compaction_candidate_zone "${ZONE}"
        expected_dest_zone="$(stat_value zone_compaction_candidate_dest_zone)" ||
            fail "missing zone_compaction_candidate_dest_zone"
        [[ "${expected_dest_zone}" =~ ^[0-9]+$ &&
           "${expected_dest_zone}" -ne "${ZONE}" &&
           "${expected_dest_zone}" -ne "${marker_zone}" ]] ||
            fail "invalid free-pool destination: ${expected_dest_zone}"
        log "enable auto compaction for source zone ${ZONE}"
        printf '1\n' > "${DEBUGFS}/zone_compaction_auto_run"
        while true; do
            after_count="$(stat_value zone_compaction_count)"
            pending="$(stat_value zone_compaction_auto_pending)"
            running="$(stat_value zone_compaction_auto_running)"
            last_error="$(stat_value last_zone_compaction_auto_run_error)"
            if [[ "${after_count}" -eq $((before_count + 1)) &&
                  "${pending}" -eq 0 && "${running}" -eq 0 ]]; then
                [[ "${last_error}" -eq 0 ]] ||
                    fail "auto zone compaction returned ${last_error}"
                break
            fi
            if [[ "${pending}" -eq 0 && "${running}" -eq 0 &&
                  "${last_error}" -ne 0 ]]; then
                fail "auto zone compaction returned ${last_error}"
            fi
            if [[ "${SECONDS}" -ge "${deadline}" ]]; then
                fail "auto zone compaction timed out after ${AUTO_TIMEOUT}s: count=${after_count} pending=${pending} running=${running} error=${last_error}"
            fi
            sleep 0.1
        done

        after_auto_run_count="$(stat_value zone_compaction_auto_run_count)"
        after_auto_failed_count="$(stat_value zone_compaction_auto_run_failed_count)"
        after_zone_failed_count="$(stat_value zone_compaction_failed_count)"
        last_auto_zone="$(stat_value last_zone_compaction_auto_run_zone)"
        [[ "${after_auto_run_count}" -eq $((before_auto_run_count + 1)) ]] ||
            fail "zone_compaction_auto_run_count did not increase by 1: before=${before_auto_run_count} after=${after_auto_run_count}"
        [[ "${after_auto_failed_count}" -eq "${before_auto_failed_count}" ]] ||
            fail "zone_compaction_auto_run_failed_count changed: before=${before_auto_failed_count} after=${after_auto_failed_count}"
        [[ "${after_zone_failed_count}" -eq "${before_zone_failed_count}" ]] ||
            fail "zone_compaction_failed_count changed: before=${before_zone_failed_count} after=${after_zone_failed_count}"
        [[ "${last_auto_zone}" == "${ZONE}" ]] ||
            fail "last_zone_compaction_auto_run_zone: expected ${ZONE}, got ${last_auto_zone}"
        log "PASS: auto run ${before_auto_run_count} -> ${after_auto_run_count}, zone=${last_auto_zone}, no failures"
    else
        log "compact source zone ${ZONE}"
        if ! printf '%s\n' "${ZONE}" > "${DEBUGFS}/compact_zone"; then
            fail "compact_zone failed; recreate a fresh dm target with an empty free-zone pool destination"
        fi
        after_count="$(stat_value zone_compaction_count)"
        expected_dest_zone="$(stat_value last_zone_compaction_dest_zone0)" ||
            fail "missing last_zone_compaction_dest_zone0"
        [[ "${expected_dest_zone}" =~ ^[0-9]+$ &&
           "${expected_dest_zone}" -ne "${ZONE}" &&
           "${expected_dest_zone}" -ne "${marker_zone}" ]] ||
            fail "invalid free-pool destination: ${expected_dest_zone}"
    fi
    [[ "${after_count}" -eq $((before_count + 1)) ]] ||
        fail "zone_compaction_count did not increase by 1: before=${before_count} after=${after_count}"
    log "PASS: zone_compaction_count ${before_count} -> ${after_count}"

    assert_stat_equals last_zone_compaction_source_zone "${ZONE}"
    assert_stat_equals last_zone_compaction_dest_zone0 "${expected_dest_zone}"
    assert_stat_equals last_zone_compaction_dest_zone1 none
    assert_stat_equals last_zone_compaction_input_entries "${TOTAL_ITEMS}"
    assert_stat_equals last_zone_compaction_live_entries "$((TOTAL_ITEMS - 4))"
    assert_stat_equals last_zone_compaction_skipped_entries 4
    assert_stat_equals last_zone_compaction_failed_entries 0
    assert_stat_equals last_zone_compaction_error 0
    assert_stat_equals last_zone_compaction_copied_entries "$((TOTAL_ITEMS - 4))"

    assert_read_equals "${key_bottom_first}" "${pattern_11}" "after compact bottom first marker"
    assert_read_equals "${key_bottom_last}" "${pattern_22}" "after compact bottom last marker"
    assert_read_equals "${key_top_first}" "${pattern_33}" "after compact top first marker"
    assert_read_equals "${key_top_last}" "${pattern_44}" "after compact top last marker"

    log "verify append placement and free-pool GC destination"
    assert_latest_pba_in_bottom_zone "${key_bottom_first}" "${marker_zone}" \
        "key ${key_bottom_first} remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_bottom_last}" "${marker_zone}" \
        "key ${key_bottom_last} remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_top_first}" "${marker_zone}" \
        "key ${key_top_first} remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_top_last}" "${marker_zone}" \
        "key ${key_top_last} remains in active append zone ${marker_zone}"
    assert_latest_pba_in_bottom_zone "${key_gc_probe}" "${expected_dest_zone}" \
        "untouched source key ${key_gc_probe} moved to free-pool zone ${expected_dest_zone}"

    log "PASS: zone GC copied only live source records and reclaimed the victim"
}

main "$@"

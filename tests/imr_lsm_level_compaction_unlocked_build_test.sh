#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
BASE_KEY="${IMR_LSM_UNLOCKED_BUILD_BASE_KEY:-8192}"
BUILD_DELAY_MS="${IMR_LSM_UNLOCKED_BUILD_DELAY_MS:-5000}"
PHASE_TIMEOUT="${IMR_LSM_UNLOCKED_BUILD_PHASE_TIMEOUT:-15}"
FOREGROUND_TIMEOUT="${IMR_LSM_UNLOCKED_BUILD_FOREGROUND_TIMEOUT:-2}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
COMPACTION_THRESHOLD=16
TRIGGER_BLOCKS=16
FOREGROUND_KEY_OFFSET=64

TMPDIR=""
TRIGGER_PID=""
ORIGINAL_THRESHOLD=""
ORIGINAL_BUILD_DELAY_MS=""

log()
{
    printf '[imr-lsm unlocked-build] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm unlocked-build] FAIL: %s\n' "$*" >&2
    exit 1
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

    value="$(stat_value "${key}")" || fail "missing stat ${key}"
    case "${value}" in
        ''|*[!0-9]*)
            fail "stat ${key} is not numeric: ${value}"
            ;;
    esac
    printf '%s\n' "${value}"
}

require_root()
{
    [[ "${EUID}" -eq 0 ]] ||
        fail "run as root, for example: sudo $0 ${DEVICE}"
}

require_device()
{
    [[ -b "${DEVICE}" ]] || fail "${DEVICE} is not a block device"
    case "${DEVICE}" in
        /dev/mapper/imrsim|/dev/mapper/imrsim[0-9]*)
            ;;
        *)
            [[ "${IMR_LSM_TEST_ALLOW_ANY_DEVICE:-0}" == "1" ]] ||
                fail "refusing to write ${DEVICE}; set IMR_LSM_TEST_ALLOW_ANY_DEVICE=1 to override"
            ;;
    esac
}

require_debugfs()
{
    local file

    [[ -d "${DEBUGFS}" ]] ||
        fail "missing ${DEBUGFS}; load dm-imrsim and mount debugfs"
    for file in stats compaction_threshold \
        level_compaction_build_delay_ms; do
        [[ -e "${DEBUGFS}/${file}" ]] ||
            fail "missing ${DEBUGFS}/${file}"
    done
}

require_tools()
{
    local tool

    for tool in awk blockdev cmp dd mktemp perl sleep sync timeout; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_parameters_and_range()
{
    local blocks
    local sectors
    local foreground_key

    imr_lsm_test_require_nonnegative_integer BASE_KEY "${BASE_KEY}"
    imr_lsm_test_require_nonnegative_integer BUILD_DELAY_MS \
        "${BUILD_DELAY_MS}"
    imr_lsm_test_require_nonnegative_integer PHASE_TIMEOUT \
        "${PHASE_TIMEOUT}"
    imr_lsm_test_require_nonnegative_integer FOREGROUND_TIMEOUT \
        "${FOREGROUND_TIMEOUT}"
    [[ "${BUILD_DELAY_MS}" -gt 0 && "${BUILD_DELAY_MS}" -le 5000 ]] ||
        fail "BUILD_DELAY_MS must be in 1..5000"
    [[ "${PHASE_TIMEOUT}" -gt 0 ]] || fail "PHASE_TIMEOUT must be > 0"
    [[ "${FOREGROUND_TIMEOUT}" -gt 0 ]] ||
        fail "FOREGROUND_TIMEOUT must be > 0"
    [[ $((FOREGROUND_TIMEOUT * 1000)) -lt "${BUILD_DELAY_MS}" ]] ||
        fail "FOREGROUND_TIMEOUT must be shorter than BUILD_DELAY_MS"

    foreground_key=$((BASE_KEY + FOREGROUND_KEY_OFFSET))
    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    blocks=$((sectors / SECTORS_PER_BLOCK))
    [[ $((BASE_KEY + TRIGGER_BLOCKS)) -le "${blocks}" ]] ||
        fail "trigger range exceeds device: base=${BASE_KEY} blocks=${blocks}"
    [[ "${foreground_key}" -lt "${blocks}" ]] ||
        fail "foreground key ${foreground_key} exceeds device blocks=${blocks}"
}

require_fresh_metadata()
{
    local key
    local value

    for key in lsm_record_insert_count compaction_count \
        level_compaction_work_run_count level_compaction_work_error_count; do
        value="$(stat_number "${key}")"
        [[ "${value}" -eq 0 ]] ||
            fail "fresh mapper required: ${key}=${value}; recreate mapper before this test"
    done
}

cleanup()
{
    set +e
    if [[ -n "${TRIGGER_PID}" ]]; then
        wait "${TRIGGER_PID}" 2>/dev/null
    fi
    if [[ -n "${ORIGINAL_BUILD_DELAY_MS}" &&
          -e "${DEBUGFS}/level_compaction_build_delay_ms" ]]; then
        printf '%s\n' "${ORIGINAL_BUILD_DELAY_MS}" \
            > "${DEBUGFS}/level_compaction_build_delay_ms"
    fi
    if [[ -n "${ORIGINAL_THRESHOLD}" &&
          -e "${DEBUGFS}/compaction_threshold" ]]; then
        printf '%s\n' "${ORIGINAL_THRESHOLD}" \
            > "${DEBUGFS}/compaction_threshold"
    fi
    if [[ -n "${TMPDIR}" && -d "${TMPDIR}" ]]; then
        rm -rf -- "${TMPDIR}"
    fi
}

make_trigger_payload()
{
    local path="$1"

    perl -e '
        my ($blocks, $block_size) = @ARGV;
        for(my $i = 0; $i < $blocks; $i++){
            print chr(($i % 251) + 1) x $block_size;
        }
    ' "${TRIGGER_BLOCKS}" "${BLOCK_SIZE}" > "${path}"
}

wait_for_build_phase()
{
    local deadline=$((SECONDS + PHASE_TIMEOUT))
    local phase

    while true; do
        phase="$(stat_number level_compaction_phase)"
        if [[ "${phase}" -eq 2 ]]; then
            return 0
        fi
        if [[ "${SECONDS}" -ge "${deadline}" ]]; then
            fail "did not observe unlocked build phase (phase=2) within ${PHASE_TIMEOUT}s; last phase=${phase}"
        fi
        sleep 0.05
    done
}

assert_delta_equals()
{
    local key="$1"
    local before="$2"
    local expected="$3"
    local after
    local delta

    after="$(stat_number "${key}")"
    delta=$((after - before))
    [[ "${delta}" -eq "${expected}" ]] ||
        fail "${key}: expected delta ${expected}, got ${delta} (before=${before} after=${after})"
    log "PASS: ${key} delta=${delta}"
}

main()
{
    require_root
    require_device
    require_debugfs
    require_tools
    imr_lsm_test_safety_begin "${DEVICE}"
    require_parameters_and_range
    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"
    require_fresh_metadata

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT

    local trigger_payload="${TMPDIR}/trigger.bin"
    local foreground_payload="${TMPDIR}/foreground.bin"
    local foreground_read="${TMPDIR}/foreground-read.bin"
    local foreground_key=$((BASE_KEY + FOREGROUND_KEY_OFFSET))
    local before_compaction
    local before_error
    local before_conflict
    local before_prepare
    local before_build
    local before_publish
    local build_ns
    local lock_max_ns
    local delay_ns=$((BUILD_DELAY_MS * 1000000))

    ORIGINAL_THRESHOLD="$(tr -d '[:space:]' < \
        "${DEBUGFS}/compaction_threshold")"
    ORIGINAL_BUILD_DELAY_MS="$(tr -d '[:space:]' < \
        "${DEBUGFS}/level_compaction_build_delay_ms")"
    printf '%s\n' "${COMPACTION_THRESHOLD}" \
        > "${DEBUGFS}/compaction_threshold"
    printf '%s\n' "${BUILD_DELAY_MS}" \
        > "${DEBUGFS}/level_compaction_build_delay_ms"

    before_compaction="$(stat_number compaction_count)"
    before_error="$(stat_number level_compaction_work_error_count)"
    before_conflict="$(stat_number level_compaction_publish_conflict_count)"
    before_prepare="$(stat_number level_compaction_prepare_time_count)"
    before_build="$(stat_number level_compaction_build_time_count)"
    before_publish="$(stat_number level_compaction_publish_time_count)"

    make_trigger_payload "${trigger_payload}"
    perl -e 'print chr(0xa5) x $ARGV[0]' \
        "${BLOCK_SIZE}" > "${foreground_payload}"

    log "trigger L0 compaction with ${TRIGGER_BLOCKS} blocks; injected build delay=${BUILD_DELAY_MS}ms"
    dd if="${trigger_payload}" of="${DEVICE}" bs="${BLOCK_SIZE}" \
        seek="${BASE_KEY}" count="${TRIGGER_BLOCKS}" conv=notrunc \
        oflag=direct status=none &
    TRIGGER_PID=$!

    wait_for_build_phase
    log "observed level_compaction_phase=2; issue foreground write/read with ${FOREGROUND_TIMEOUT}s limits"
    timeout "${FOREGROUND_TIMEOUT}s" dd if="${foreground_payload}" \
        of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${foreground_key}" count=1 \
        conv=notrunc oflag=direct status=none ||
        fail "foreground write blocked during the unlocked build phase"
    [[ "$(stat_number level_compaction_phase)" -eq 2 ]] ||
        fail "build phase ended before the concurrent read; increase BUILD_DELAY_MS"
    timeout "${FOREGROUND_TIMEOUT}s" dd if="${DEVICE}" \
        of="${foreground_read}" bs="${BLOCK_SIZE}" skip="${foreground_key}" \
        count=1 iflag=direct status=none ||
        fail "foreground read blocked during the unlocked build phase"
    cmp -s "${foreground_payload}" "${foreground_read}" ||
        fail "foreground payload mismatch during level compaction build"
    log "PASS: foreground write/read completed while build delay was active"

    wait "${TRIGGER_PID}" || fail "trigger write failed"
    TRIGGER_PID=""
    sync
    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"

    assert_delta_equals compaction_count "${before_compaction}" 1
    assert_delta_equals level_compaction_work_error_count "${before_error}" 0
    assert_delta_equals level_compaction_publish_conflict_count \
        "${before_conflict}" 0
    assert_delta_equals level_compaction_prepare_time_count \
        "${before_prepare}" 1
    assert_delta_equals level_compaction_build_time_count "${before_build}" 1
    assert_delta_equals level_compaction_publish_time_count \
        "${before_publish}" 1

    build_ns="$(stat_number last_level_compaction_build_ns)"
    [[ "${build_ns}" -ge "${delay_ns}" ]] ||
        fail "build phase ${build_ns}ns is shorter than injected delay ${delay_ns}ns"
    lock_max_ns="$(stat_number level_compaction_zone_lock_hold_max_ns)"
    [[ "${lock_max_ns}" -lt "${delay_ns}" ]] ||
        fail "zone lock max hold ${lock_max_ns}ns includes the ${delay_ns}ns build delay"
    lock_max_ns="$(stat_number level_compaction_lsm_lock_hold_max_ns)"
    [[ "${lock_max_ns}" -lt "${delay_ns}" ]] ||
        fail "LSM lock max hold ${lock_max_ns}ns includes the ${delay_ns}ns build delay"
    [[ "$(stat_number level_compaction_phase)" -eq 0 ]] ||
        fail "level compaction phase did not return to idle"

    log "PASS: prepare/build/publish completed; the injected build delay was outside both global locks"
}

main "$@"

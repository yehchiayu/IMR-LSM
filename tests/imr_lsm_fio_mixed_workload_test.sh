#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_FIO_ZONE:-1}"
KEY_OFFSET="${IMR_LSM_FIO_KEY_OFFSET:-4096}"
SIZE_BYTES="${IMR_LSM_FIO_SIZE_BYTES:-16777216}"
IOENGINE="${IMR_LSM_FIO_IOENGINE:-sync}"
IODEPTH="${IMR_LSM_FIO_IODEPTH:-1}"
RWMIXREAD="${IMR_LSM_FIO_RWMIXREAD:-60}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
BLOCKS_PER_ZONE=65536

log()
{
    printf '[imr-lsm fio-workload] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm fio-workload] FAIL: %s\n' "$*" >&2
    exit 1
}

require_root()
{
    [[ "${EUID}" -eq 0 ]] ||
        fail "run as root, for example: sudo env IMR_LSM_TEST_DESTRUCTIVE=1 $0 ${DEVICE}"
}

require_tools()
{
    local tool

    for tool in awk blockdev fio sync; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_debugfs()
{
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats"
}

stat_value()
{
    local key="$1"

    awk -F': ' -v key="${key}" '
        $1 == key { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "${DEBUGFS}/stats"
}

stat_number()
{
    local key="$1"
    local value

    value="$(stat_value "${key}")" ||
        fail "missing stat ${key}"
    [[ "${value}" =~ ^[0-9]+$ ]] ||
        fail "stat ${key} is not numeric: ${value}"
    printf '%s\n' "${value}"
}

have_stat()
{
    stat_value "$1" >/dev/null 2>&1
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
        fail "${label}: ${key} delta expected >= ${minimum}, got ${delta}"
    log "PASS: ${label}: ${key} +${delta} >= ${minimum}"
}

device_zone_count()
{
    local sectors

    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    printf '%s\n' "$((sectors / SECTORS_PER_BLOCK / BLOCKS_PER_ZONE))"
}

require_integer_multiple()
{
    local label="$1"
    local value="$2"
    local multiple="$3"

    imr_lsm_test_require_nonnegative_integer "${label}" "${value}"
    [[ "${value}" -gt 0 ]] ||
        fail "${label}=${value} must be > 0"
    [[ $((value % multiple)) -eq 0 ]] ||
        fail "${label}=${value} must be a multiple of ${multiple}"
}

require_test_range()
{
    local zone_count
    local size_blocks

    imr_lsm_test_require_nonnegative_integer ZONE "${ZONE}"
    imr_lsm_test_require_nonnegative_integer KEY_OFFSET "${KEY_OFFSET}"
    imr_lsm_test_require_nonnegative_integer RWMIXREAD "${RWMIXREAD}"
    imr_lsm_test_require_nonnegative_integer IODEPTH "${IODEPTH}"
    require_integer_multiple SIZE_BYTES "${SIZE_BYTES}" "${BLOCK_SIZE}"

    [[ "${IODEPTH}" -gt 0 ]] ||
        fail "IODEPTH=${IODEPTH} must be > 0"
    [[ "${RWMIXREAD}" -le 100 ]] ||
        fail "RWMIXREAD=${RWMIXREAD} must be <= 100"

    zone_count="$(device_zone_count)"
    [[ "${ZONE}" -lt "${zone_count}" ]] ||
        fail "zone ${ZONE} is outside ${DEVICE}; available zones=${zone_count}"

    size_blocks=$((SIZE_BYTES / BLOCK_SIZE))
    [[ $((KEY_OFFSET + size_blocks)) -le "${BLOCKS_PER_ZONE}" ]] ||
        fail "fio range crosses zone: offset=${KEY_OFFSET} size_blocks=${size_blocks}"
}

run_fio_phase()
{
    local name="$1"
    local rw="$2"
    local offset_bytes="$3"

    log "fio phase=${name} rw=${rw} offset=${offset_bytes} size=${SIZE_BYTES}"
    fio \
        --name="${name}" \
        --filename="${DEVICE}" \
        --direct=1 \
        --ioengine="${IOENGINE}" \
        --iodepth="${IODEPTH}" \
        --numjobs=1 \
        --bs="${BLOCK_SIZE}" \
        --offset="${offset_bytes}" \
        --size="${SIZE_BYTES}" \
        --rw="${rw}" \
        --rwmixread="${RWMIXREAD}" \
        --group_reporting
}

main()
{
    local first_key
    local offset_bytes
    local before_insert
    local before_read
    local before_delete
    local before_discard_delete=""

    require_root
    require_tools
    require_debugfs
    imr_lsm_test_safety_begin "${DEVICE}"
    require_test_range

    first_key=$((ZONE * BLOCKS_PER_ZONE + KEY_OFFSET))
    offset_bytes=$((first_key * BLOCK_SIZE))

    log "device=${DEVICE} debugfs=${DEBUGFS} zone=${ZONE} first_key=${first_key}"
    log "range size=${SIZE_BYTES} bytes ioengine=${IOENGINE}"

    before_insert="$(stat_number lsm_record_insert_count)"
    before_read="$(stat_number read_lookup_count)"
    before_delete="$(stat_number delete_count)"
    if have_stat discard_delete_count; then
        before_discard_delete="$(stat_number discard_delete_count)"
    fi

    run_fio_phase imr_lsm_randwrite randwrite "${offset_bytes}"
    run_fio_phase imr_lsm_randrw randrw "${offset_bytes}"
    run_fio_phase imr_lsm_randread randread "${offset_bytes}"
    run_fio_phase imr_lsm_randtrim randtrim "${offset_bytes}"
    sync
    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"

    assert_counter_delta_ge lsm_record_insert_count "${before_insert}" 1 \
        "fio randwrite/randrw publish write mappings"
    assert_counter_delta_ge read_lookup_count "${before_read}" 1 \
        "fio randread/randrw exercise read path"
    assert_counter_delta_ge delete_count "${before_delete}" 1 \
        "fio randtrim publishes trim tombstones"
    if [[ -n "${before_discard_delete}" ]]; then
        assert_counter_delta_ge discard_delete_count "${before_discard_delete}" 1 \
            "fio randtrim reaches discard delete path"
    fi

    log "PASS: fio randwrite/randrw/randtrim/randread workload completed"
}

main "$@"

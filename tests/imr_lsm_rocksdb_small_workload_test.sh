#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
LDB_BIN="${IMR_LSM_ROCKSDB_LDB:-ldb}"
KEY_COUNT="${IMR_LSM_ROCKSDB_KEYS:-8}"
FSTRIM_LENGTH_BYTES="${IMR_LSM_ROCKSDB_FSTRIM_LENGTH_BYTES:-16777216}"
RUN_FSTRIM="${IMR_LSM_ROCKSDB_FSTRIM:-1}"
MOUNT_OPTIONS="${IMR_LSM_ROCKSDB_MOUNT_OPTIONS:-noatime,nodiratime}"
MKFS_EXT_OPTS="${IMR_LSM_ROCKSDB_MKFS_EXT_OPTS:-nodiscard,lazy_itable_init=0,lazy_journal_init=0}"
MKFS_BLOCK_SIZE="${IMR_LSM_ROCKSDB_MKFS_BLOCK_SIZE:-4096}"
MKFS_BLOCKS="${IMR_LSM_ROCKSDB_MKFS_BLOCKS:-16384}"

BLOCK_SIZE=4096
TMPDIR=""
MNT=""
DB_PATH=""

log()
{
    printf '[imr-lsm rocksdb-workload] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm rocksdb-workload] FAIL: %s\n' "$*" >&2
    exit 1
}

cleanup()
{
    set +e
    if [[ -n "${MNT}" ]] && mountpoint -q "${MNT}"; then
        umount "${MNT}" || umount -l "${MNT}"
    fi
    if [[ -n "${MNT}" && -d "${MNT}" ]]; then
        rmdir "${MNT}"
    fi
    if [[ -n "${TMPDIR}" && -d "${TMPDIR}" ]]; then
        rm -rf -- "${TMPDIR}"
    fi
}

require_root()
{
    [[ "${EUID}" -eq 0 ]] ||
        fail "run as root, for example: sudo env IMR_LSM_TEST_DESTRUCTIVE=1 $0 ${DEVICE}"
}

require_tools()
{
    local tool

    for tool in awk blockdev mkdir mkfs.ext4 mktemp mount mountpoint \
        rm rmdir sync umount; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        command -v fstrim >/dev/null 2>&1 ||
            fail "missing required tool: fstrim"
    fi
    command -v "${LDB_BIN}" >/dev/null 2>&1 ||
        fail "missing RocksDB ldb tool: ${LDB_BIN}; set IMR_LSM_ROCKSDB_LDB=/path/to/ldb"
}

require_debugfs()
{
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats"
}

require_test_range()
{
    local device_bytes
    local filesystem_bytes

    imr_lsm_test_require_nonnegative_integer KEY_COUNT "${KEY_COUNT}"
    imr_lsm_test_require_nonnegative_integer MKFS_BLOCK_SIZE \
        "${MKFS_BLOCK_SIZE}"
    imr_lsm_test_require_nonnegative_integer MKFS_BLOCKS "${MKFS_BLOCKS}"
    [[ "${KEY_COUNT}" -ge 8 ]] ||
        fail "KEY_COUNT=${KEY_COUNT} must be >= 8"
    [[ "${MKFS_BLOCK_SIZE}" -eq "${BLOCK_SIZE}" ]] ||
        fail "MKFS_BLOCK_SIZE=${MKFS_BLOCK_SIZE} must match the 4 KiB IMR-LSM block size"
    [[ "${MKFS_BLOCKS}" -gt 0 ]] ||
        fail "MKFS_BLOCKS=${MKFS_BLOCKS} must be > 0"

    device_bytes="$(blockdev --getsize64 "${DEVICE}")" ||
        fail "cannot read byte size for ${DEVICE}"
    filesystem_bytes=$((MKFS_BLOCKS * MKFS_BLOCK_SIZE))
    [[ "${device_bytes}" -ge "${filesystem_bytes}" ]] ||
        fail "device is smaller than requested filesystem: device=${device_bytes} fs=${filesystem_bytes}"

    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        imr_lsm_test_require_nonnegative_integer FSTRIM_LENGTH_BYTES \
            "${FSTRIM_LENGTH_BYTES}"
        [[ "${FSTRIM_LENGTH_BYTES}" -gt 0 ]] ||
            fail "FSTRIM_LENGTH_BYTES=${FSTRIM_LENGTH_BYTES} must be > 0"
        [[ $((FSTRIM_LENGTH_BYTES % BLOCK_SIZE)) -eq 0 ]] ||
            fail "FSTRIM_LENGTH_BYTES=${FSTRIM_LENGTH_BYTES} must be 4 KiB aligned"
        [[ "${filesystem_bytes}" -ge "${FSTRIM_LENGTH_BYTES}" ]] ||
            fail "filesystem is smaller than requested fstrim window: fs=${filesystem_bytes} fstrim=${FSTRIM_LENGTH_BYTES}"
    fi
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

format_device()
{
    local output

    log "mkfs.ext4 -b ${MKFS_BLOCK_SIZE} ${DEVICE} ${MKFS_BLOCKS}"
    if output="$(
        mkfs.ext4 -F -q -b "${MKFS_BLOCK_SIZE}" -E "${MKFS_EXT_OPTS}" \
            "${DEVICE}" "${MKFS_BLOCKS}" 2>&1
    )"; then
        return
    fi

    printf '%s\n' "${output}" >&2
    if [[ "${output}" == *"short write"* ||
          "${output}" == *"short read"* ||
          "${output}" == *"could not erase sector"* ]]; then
        fail "mkfs.ext4 issued sub-4KiB metadata I/O that IMR-LSM partial-block RMW did not satisfy"
    fi
    fail "mkfs.ext4 failed; override IMR_LSM_ROCKSDB_MKFS_EXT_OPTS if this e2fsprogs version needs different options"
}

mount_device()
{
    MNT="$(mktemp -d)"
    if [[ -n "${MOUNT_OPTIONS}" ]]; then
        mount -o "${MOUNT_OPTIONS}" "${DEVICE}" "${MNT}"
    else
        mount "${DEVICE}" "${MNT}"
    fi
    DB_PATH="${MNT}/rocksdb-smoke"
    log "mounted ${DEVICE} at ${MNT}"
}

remount_device()
{
    umount "${MNT}"
    if [[ -n "${MOUNT_OPTIONS}" ]]; then
        mount -o "${MOUNT_OPTIONS}" "${DEVICE}" "${MNT}"
    else
        mount "${DEVICE}" "${MNT}"
    fi
    DB_PATH="${MNT}/rocksdb-smoke"
    log "remounted ${DEVICE} at ${MNT}"
}

ldb_cmd()
{
    "${LDB_BIN}" --db="${DB_PATH}" "$@"
}

ldb_create_cmd()
{
    "${LDB_BIN}" --db="${DB_PATH}" --create_if_missing "$@"
}

put_key()
{
    local key="$1"
    local value="$2"
    local output

    if ! output="$(ldb_create_cmd put "${key}" "${value}" 2>&1)"; then
        fail "ldb put ${key} failed: ${output}"
    fi
}

delete_key()
{
    local key="$1"
    local output

    if ! output="$(ldb_cmd delete "${key}" 2>&1)"; then
        fail "ldb delete ${key} failed: ${output}"
    fi
}

assert_get_value()
{
    local key="$1"
    local expected="$2"
    local label="$3"
    local output

    output="$(ldb_cmd get "${key}" 2>&1)" ||
        fail "${label}: ldb get ${key} failed: ${output}"
    [[ "${output}" == *"${expected}"* ]] ||
        fail "${label}: expected ${expected} for ${key}, got: ${output}"
    log "PASS: ${label}"
}

assert_get_not_found()
{
    local key="$1"
    local label="$2"
    local output
    local lower

    if output="$(ldb_cmd get "${key}" 2>&1)"; then
        lower="${output,,}"
        [[ "${lower}" == *"notfound"* || "${lower}" == *"not found"* ]] ||
            fail "${label}: deleted key ${key} returned data: ${output}"
    else
        lower="${output,,}"
        [[ "${lower}" == *"notfound"* || "${lower}" == *"not found"* ]] ||
            fail "${label}: ldb get ${key} failed without NotFound evidence: ${output}"
    fi
    log "PASS: ${label}"
}

run_rocksdb_workload()
{
    local i
    local key
    local value

    mkdir -p "${DB_PATH}"

    log "ldb put ${KEY_COUNT} keys"
    for ((i = 0; i < KEY_COUNT; i++)); do
        printf -v key 'key-%04d' "${i}"
        printf -v value 'value-a-%04d' "${i}"
        put_key "${key}" "${value}"
    done
    sync

    log "ldb update every second key"
    for ((i = 0; i < KEY_COUNT; i += 2)); do
        printf -v key 'key-%04d' "${i}"
        printf -v value 'value-b-%04d' "${i}"
        put_key "${key}" "${value}"
    done
    sync

    log "ldb delete every third key"
    for ((i = 0; i < KEY_COUNT; i += 3)); do
        printf -v key 'key-%04d' "${i}"
        delete_key "${key}"
    done
    sync

    if ldb_cmd compact >/dev/null 2>"${TMPDIR}/ldb-compact.err"; then
        log "PASS: ldb compact completed"
    else
        log "SKIP: ldb compact failed; continuing with put/update/delete readback"
    fi
    sync
}

verify_rocksdb_readback()
{
    local i
    local key
    local expected

    for ((i = 0; i < KEY_COUNT; i++)); do
        printf -v key 'key-%04d' "${i}"
        if [[ $((i % 3)) -eq 0 ]]; then
            assert_get_not_found "${key}" "deleted key ${key} stays deleted"
        elif [[ $((i % 2)) -eq 0 ]]; then
            printf -v expected 'value-b-%04d' "${i}"
            assert_get_value "${key}" "${expected}" \
                "updated key ${key} reads latest value"
        else
            printf -v expected 'value-a-%04d' "${i}"
            assert_get_value "${key}" "${expected}" \
                "untouched key ${key} reads original value"
        fi
    done
}

main()
{
    local before_insert
    local before_delete
    local before_discard_delete=""

    require_root
    require_tools
    require_debugfs
    imr_lsm_test_safety_begin "${DEVICE}"
    require_test_range

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT

    before_insert="$(stat_number lsm_record_insert_count)"

    format_device
    mount_device
    run_rocksdb_workload
    verify_rocksdb_readback

    assert_counter_delta_ge lsm_record_insert_count "${before_insert}" 1 \
        "RocksDB filesystem workload publishes block writes"

    if [[ "${RUN_FSTRIM}" == "1" ]]; then
        before_delete="$(stat_number delete_count)"
        if have_stat discard_delete_count; then
            before_discard_delete="$(stat_number discard_delete_count)"
        fi
        log "fstrim offset=0 length=${FSTRIM_LENGTH_BYTES}"
        fstrim -o 0 -l "${FSTRIM_LENGTH_BYTES}" -m "${BLOCK_SIZE}" "${MNT}"
        sync
        assert_counter_delta_ge delete_count "${before_delete}" 1 \
            "post-RocksDB fstrim publishes IMR-LSM tombstones"
        if [[ -n "${before_discard_delete}" ]]; then
            assert_counter_delta_ge discard_delete_count "${before_discard_delete}" 1 \
                "post-RocksDB fstrim reaches discard delete path"
        fi
        remount_device
        verify_rocksdb_readback
    fi

    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"
    log "PASS: RocksDB put/update/delete/readback workload completed"
}

main "$@"

#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
FSTRIM_LENGTH_BYTES="${IMR_LSM_FS_FSTRIM_LENGTH_BYTES:-4194304}"
MOUNT_OPTIONS="${IMR_LSM_FS_MOUNT_OPTIONS:-noatime,nodiratime}"
MKFS_EXT_OPTS="${IMR_LSM_FS_MKFS_EXT_OPTS:-nodiscard,lazy_itable_init=0,lazy_journal_init=0}"
MKFS_BLOCK_SIZE="${IMR_LSM_FS_MKFS_BLOCK_SIZE:-4096}"
MKFS_BLOCKS="${IMR_LSM_FS_MKFS_BLOCKS:-16384}"

BLOCK_SIZE=4096
TMPDIR=""
MNT=""

log()
{
    printf '[imr-lsm ext4-workload] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm ext4-workload] FAIL: %s\n' "$*" >&2
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

    for tool in awk blockdev cmp cp fstrim mkdir mkfs.ext4 mktemp mount \
        mountpoint perl rm rmdir sync test umount; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
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

    imr_lsm_test_require_nonnegative_integer FSTRIM_LENGTH_BYTES \
        "${FSTRIM_LENGTH_BYTES}"
    imr_lsm_test_require_nonnegative_integer MKFS_BLOCK_SIZE \
        "${MKFS_BLOCK_SIZE}"
    imr_lsm_test_require_nonnegative_integer MKFS_BLOCKS "${MKFS_BLOCKS}"
    [[ "${FSTRIM_LENGTH_BYTES}" -gt 0 ]] ||
        fail "FSTRIM_LENGTH_BYTES=${FSTRIM_LENGTH_BYTES} must be > 0"
    [[ $((FSTRIM_LENGTH_BYTES % BLOCK_SIZE)) -eq 0 ]] ||
        fail "FSTRIM_LENGTH_BYTES=${FSTRIM_LENGTH_BYTES} must be 4 KiB aligned"
    [[ "${MKFS_BLOCK_SIZE}" -eq "${BLOCK_SIZE}" ]] ||
        fail "MKFS_BLOCK_SIZE=${MKFS_BLOCK_SIZE} must match the 4 KiB IMR-LSM block size"
    [[ "${MKFS_BLOCKS}" -gt 0 ]] ||
        fail "MKFS_BLOCKS=${MKFS_BLOCKS} must be > 0"

    device_bytes="$(blockdev --getsize64 "${DEVICE}")" ||
        fail "cannot read byte size for ${DEVICE}"
    filesystem_bytes=$((MKFS_BLOCKS * MKFS_BLOCK_SIZE))
    [[ "${device_bytes}" -ge "${filesystem_bytes}" ]] ||
        fail "device is smaller than requested filesystem: device=${device_bytes} fs=${filesystem_bytes}"
    [[ "${filesystem_bytes}" -ge "${FSTRIM_LENGTH_BYTES}" ]] ||
        fail "filesystem is smaller than requested fstrim window: fs=${filesystem_bytes} fstrim=${FSTRIM_LENGTH_BYTES}"
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

make_pattern()
{
    local byte_value="$1"
    local byte_count="$2"
    local output="$3"

    perl -e 'print chr(hex($ARGV[0])) x $ARGV[1]' \
        "${byte_value}" "${byte_count}" > "${output}"
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
    fail "mkfs.ext4 failed; override IMR_LSM_FS_MKFS_EXT_OPTS if this e2fsprogs version needs different options"
}

mount_device()
{
    MNT="$(mktemp -d)"
    if [[ -n "${MOUNT_OPTIONS}" ]]; then
        mount -o "${MOUNT_OPTIONS}" "${DEVICE}" "${MNT}"
    else
        mount "${DEVICE}" "${MNT}"
    fi
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
    log "remounted ${DEVICE} at ${MNT}"
}

assert_file_equals()
{
    local path="$1"
    local expected="$2"
    local label="$3"

    cmp -s "${expected}" "${path}" ||
        fail "${label}: payload mismatch for ${path}"
    log "PASS: ${label}"
}

main()
{
    local workload_dir
    local before_delete
    local before_discard_delete=""

    require_root
    require_tools
    require_debugfs
    imr_lsm_test_safety_begin "${DEVICE}"
    require_test_range

    TMPDIR="$(mktemp -d)"
    trap cleanup EXIT

    make_pattern 41 131072 "${TMPDIR}/alpha-v1.bin"
    make_pattern 42 131072 "${TMPDIR}/alpha-v2.bin"
    make_pattern 43 262144 "${TMPDIR}/delete-me.bin"
    make_pattern 44 65536 "${TMPDIR}/nested.bin"

    format_device
    mount_device

    workload_dir="${MNT}/imr-lsm-workload"
    mkdir -p "${workload_dir}/nested"

    cp "${TMPDIR}/alpha-v1.bin" "${workload_dir}/alpha.bin"
    cp "${TMPDIR}/delete-me.bin" "${workload_dir}/delete-me.bin"
    cp "${TMPDIR}/nested.bin" "${workload_dir}/nested/live.bin"
    sync
    assert_file_equals "${workload_dir}/alpha.bin" "${TMPDIR}/alpha-v1.bin" \
        "created file alpha reads back"
    assert_file_equals "${workload_dir}/delete-me.bin" "${TMPDIR}/delete-me.bin" \
        "created file delete-me reads back"

    cp "${TMPDIR}/alpha-v2.bin" "${workload_dir}/alpha.bin"
    rm -f "${workload_dir}/delete-me.bin"
    sync
    assert_file_equals "${workload_dir}/alpha.bin" "${TMPDIR}/alpha-v2.bin" \
        "updated file alpha reads back"
    [[ ! -e "${workload_dir}/delete-me.bin" ]] ||
        fail "deleted file still exists"
    assert_file_equals "${workload_dir}/nested/live.bin" "${TMPDIR}/nested.bin" \
        "unrelated nested file survives delete"

    before_delete="$(stat_number delete_count)"
    if have_stat discard_delete_count; then
        before_discard_delete="$(stat_number discard_delete_count)"
    fi

    log "fstrim offset=0 length=${FSTRIM_LENGTH_BYTES}"
    fstrim -o 0 -l "${FSTRIM_LENGTH_BYTES}" -m "${BLOCK_SIZE}" "${MNT}"
    sync

    assert_counter_delta_ge delete_count "${before_delete}" 1 \
        "filesystem fstrim publishes IMR-LSM tombstones"
    if [[ -n "${before_discard_delete}" ]]; then
        assert_counter_delta_ge discard_delete_count "${before_discard_delete}" 1 \
            "filesystem fstrim reaches discard delete path"
    fi

    remount_device
    workload_dir="${MNT}/imr-lsm-workload"
    assert_file_equals "${workload_dir}/alpha.bin" "${TMPDIR}/alpha-v2.bin" \
        "updated file survives fstrim"
    assert_file_equals "${workload_dir}/nested/live.bin" "${TMPDIR}/nested.bin" \
        "nested live file survives fstrim"
    [[ ! -e "${workload_dir}/delete-me.bin" ]] ||
        fail "deleted file reappeared after fstrim"

    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"
    log "PASS: ext4 create/update/delete/fstrim workload completed"
}

main "$@"

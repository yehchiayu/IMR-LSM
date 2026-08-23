#!/usr/bin/env bash
set -euo pipefail

TEST_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${TEST_SCRIPT_DIR}/imr_lsm_test_safety.bash"

DEVICE="${1:-/dev/mapper/imrsim}"
DEBUGFS="${IMR_LSM_DEBUGFS:-/sys/kernel/debug/imrsim_lsm}"
ZONE="${IMR_LSM_BOUNDARY_ZONE:-0}"
KEY_OFFSET="${IMR_LSM_BOUNDARY_KEY_OFFSET:-4096}"

BLOCK_SIZE=4096
SECTORS_PER_BLOCK=8
BLOCKS_PER_ZONE=65536

log()
{
    printf '[imr-lsm boundary] %s\n' "$*"
}

fail()
{
    printf '[imr-lsm boundary] FAIL: %s\n' "$*" >&2
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

    for tool in awk blkdiscard blockdev cat cmp dd lsblk mktemp perl sync; do
        command -v "${tool}" >/dev/null 2>&1 ||
            fail "missing required tool: ${tool}"
    done
}

require_debugfs()
{
    [[ -r "${DEBUGFS}/stats" ]] ||
        fail "missing readable ${DEBUGFS}/stats"
}

device_zone_count()
{
    local sectors

    sectors="$(blockdev --getsz "${DEVICE}")" ||
        fail "cannot read sector count for ${DEVICE}"
    printf '%s\n' "$((sectors / SECTORS_PER_BLOCK / BLOCKS_PER_ZONE))"
}

require_test_range()
{
    local zone_count

    imr_lsm_test_require_nonnegative_integer ZONE "${ZONE}"
    imr_lsm_test_require_nonnegative_integer KEY_OFFSET "${KEY_OFFSET}"
    zone_count="$(device_zone_count)"

    [[ "${ZONE}" -lt $((zone_count - 1)) ]] ||
        fail "zone-boundary coverage needs ZONE+1; zone=${ZONE}, available zones=${zone_count}"
    [[ $((KEY_OFFSET % 2)) -eq 0 ]] ||
        fail "KEY_OFFSET must be even for the in-zone 8 KiB request"
    [[ $((KEY_OFFSET + 64)) -lt "${BLOCKS_PER_ZONE}" ]] ||
        fail "KEY_OFFSET leaves no room for the observer block"
}

require_queue_contract()
{
    local kernel_name
    local logical_block_size
    local discard_granularity
    local discard_granularity_path

    logical_block_size="$(blockdev --getss "${DEVICE}")" ||
        fail "cannot read logical block size for ${DEVICE}"
    [[ "${logical_block_size}" -le "${BLOCK_SIZE}" &&
       $((BLOCK_SIZE % logical_block_size)) -eq 0 ]] ||
        fail "logical block size must divide 4 KiB, got ${logical_block_size}"

    kernel_name="$(lsblk -dnro KNAME "${DEVICE}" | tr -d '[:space:]')" ||
        fail "cannot resolve kernel block name for ${DEVICE}"
    [[ -n "${kernel_name}" ]] ||
        fail "empty kernel block name for ${DEVICE}"
    discard_granularity_path="/sys/class/block/${kernel_name}/queue/discard_granularity"
    [[ -r "${discard_granularity_path}" ]] ||
        fail "missing ${discard_granularity_path}"
    discard_granularity="$(<"${discard_granularity_path}")"
    [[ "${discard_granularity}" =~ ^[0-9]+$ ]] ||
        fail "invalid discard granularity: ${discard_granularity}"
    [[ "${discard_granularity}" -ge "${BLOCK_SIZE}" &&
       $((discard_granularity % BLOCK_SIZE)) -eq 0 ]] ||
        fail "discard granularity must be a 4 KiB multiple, got ${discard_granularity}"

    log "queue contract: logical_block_size=${logical_block_size} discard_granularity=${discard_granularity}"
}

stat_number()
{
    local key="$1"
    local value

    value="$(
        awk -F': ' -v key="${key}" '
            $1 == key { print $2; found = 1; exit }
            END { if (!found) exit 1 }
        ' "${DEBUGFS}/stats"
    )" || fail "missing stat ${key}"
    [[ "${value}" =~ ^[0-9]+$ ]] ||
        fail "stat ${key} is not numeric: ${value}"
    printf '%s\n' "${value}"
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
        fail "${label}: ${key} delta expected ${expected}, got ${delta}"
    log "PASS: ${label}: ${key} delta=${delta}"
}

make_pattern()
{
    local byte_value="$1"
    local byte_count="$2"
    local output="$3"

    perl -e 'print chr($ARGV[0]) x $ARGV[1]' \
        "${byte_value}" "${byte_count}" > "${output}"
}

discard_block()
{
    local key="$1"

    blkdiscard -o "$((key * BLOCK_SIZE))" -l "${BLOCK_SIZE}" "${DEVICE}" ||
        fail "cannot clear key=${key} before boundary test"
}

write_block()
{
    local key="$1"
    local input="$2"

    dd if="${input}" of="${DEVICE}" bs="${BLOCK_SIZE}" seek="${key}" \
        count=1 conv=notrunc oflag=direct status=none
}

read_block()
{
    local key="$1"
    local output="$2"

    dd if="${DEVICE}" of="${output}" bs="${BLOCK_SIZE}" skip="${key}" \
        count=1 iflag=direct status=none
}

write_pair_one_request()
{
    local first_key="$1"
    local input="$2"

    dd if="${input}" of="${DEVICE}" bs="$((2 * BLOCK_SIZE))" \
        seek="$((first_key * BLOCK_SIZE))" count=1 conv=notrunc \
        oflag=direct,seek_bytes status=none
}

read_pair_one_request()
{
    local first_key="$1"
    local output="$2"

    dd if="${DEVICE}" of="${output}" bs="$((2 * BLOCK_SIZE))" \
        skip="$((first_key * BLOCK_SIZE))" count=1 \
        iflag=direct,skip_bytes status=none
}

assert_block_equals()
{
    local key="$1"
    local expected="$2"
    local label="$3"
    local output="${TMPDIR}/read-block-${key}.bin"

    read_block "${key}" "${output}" ||
        fail "${label}: read failed for key=${key}"
    cmp -s "${expected}" "${output}" ||
        fail "${label}: payload mismatch for key=${key}"
    log "PASS: ${label}"
}

assert_pair_equals()
{
    local first_key="$1"
    local expected="$2"
    local label="$3"
    local output="${TMPDIR}/read-pair-${first_key}.bin"

    read_pair_one_request "${first_key}" "${output}" ||
        fail "${label}: 8 KiB read failed for first_key=${first_key}"
    cmp -s "${expected}" "${output}" ||
        fail "${label}: 8 KiB payload mismatch for first_key=${first_key}"
    log "PASS: ${label}"
}

assert_partial_data_io_rmw()
{
    local first_key="$1"
    local expected_pair="$2"
    local before_insert
    local before_partial_rmw
    local before_partial_rmw_ns
    local after_partial_rmw_ns
    local output="${TMPDIR}/partial-read.bin"

    before_partial_rmw="$(stat_number partial_rmw_count)"
    before_partial_rmw_ns="$(stat_number partial_rmw_total_ns)"
    before_insert="$(stat_number lsm_record_insert_count)"
    dd if="${expected_pair}" of="${TMPDIR}/partial-first-expected.bin" \
        bs="${BLOCK_SIZE}" count=1 status=none
    dd if="${TMPDIR}/partial-512.bin" \
        of="${TMPDIR}/partial-first-expected.bin" bs=512 count=1 \
        conv=notrunc status=none
    dd if="${TMPDIR}/partial-first-expected.bin" \
        of="${TMPDIR}/partial-pair-expected.bin" bs="${BLOCK_SIZE}" \
        count=1 status=none
    dd if="${expected_pair}" of="${TMPDIR}/partial-pair-expected.bin" \
        bs="${BLOCK_SIZE}" skip=1 seek=1 count=1 conv=notrunc status=none

    dd if="${TMPDIR}/partial-512.bin" of="${DEVICE}" bs=512 \
        seek="$((first_key * SECTORS_PER_BLOCK))" count=1 \
        conv=notrunc oflag=direct status=none
    assert_counter_delta lsm_record_insert_count "${before_insert}" 1 \
        "512-byte write publishes one RMW mapping"
    assert_pair_equals "${first_key}" "${TMPDIR}/partial-pair-expected.bin" \
        "512-byte write preserves untouched bytes"

    before_insert="$(stat_number lsm_record_insert_count)"
    dd if="${TMPDIR}/partial-first-expected.bin" \
        of="${TMPDIR}/cross-first-expected.bin" bs="${BLOCK_SIZE}" \
        count=1 status=none
    dd if="${TMPDIR}/partial-1024.bin" \
        of="${TMPDIR}/cross-first-expected.bin" bs=512 skip=0 seek=7 \
        count=1 conv=notrunc status=none
    dd if="${expected_pair}" of="${TMPDIR}/cross-second-expected.bin" \
        bs="${BLOCK_SIZE}" skip=1 count=1 status=none
    dd if="${TMPDIR}/partial-1024.bin" \
        of="${TMPDIR}/cross-second-expected.bin" bs=512 skip=1 seek=0 \
        count=1 conv=notrunc status=none
    dd if="${TMPDIR}/cross-first-expected.bin" \
        of="${TMPDIR}/cross-rmw-pair-expected.bin" bs="${BLOCK_SIZE}" \
        count=1 status=none
    dd if="${TMPDIR}/cross-second-expected.bin" \
        of="${TMPDIR}/cross-rmw-pair-expected.bin" bs="${BLOCK_SIZE}" \
        seek=1 count=1 conv=notrunc status=none

    dd if="${TMPDIR}/partial-1024.bin" of="${DEVICE}" bs=512 \
        seek="$((first_key * SECTORS_PER_BLOCK + 7))" count=2 \
        conv=notrunc oflag=direct status=none
    assert_counter_delta lsm_record_insert_count "${before_insert}" 2 \
        "cross-block partial write publishes two RMW mappings"
    assert_pair_equals "${first_key}" "${TMPDIR}/cross-rmw-pair-expected.bin" \
        "cross-block partial write preserves untouched bytes"

    dd if="${DEVICE}" of="${output}" bs=512 \
        skip="$((first_key * SECTORS_PER_BLOCK))" count=1 \
        iflag=direct status=none
    cmp -s "${TMPDIR}/partial-512.bin" "${output}" ||
        fail "512-byte read returned unexpected payload"
    log "PASS: 512-byte read succeeds through partial-block RMW path"

    assert_counter_delta partial_rmw_count "${before_partial_rmw}" 3 \
        "partial writes record one RMW diagnostic sample per affected 4K block"
    after_partial_rmw_ns="$(stat_number partial_rmw_total_ns)"
    [[ "${after_partial_rmw_ns}" -gt "${before_partial_rmw_ns}" ]] ||
        fail "partial RMW diagnostic time did not advance"
    log "PASS: partial RMW diagnostics record count and service time"
}

assert_partial_discard_safe()
{
    local first_key="$1"
    local expected_pair="$2"
    local before_delete
    local discard_result

    before_delete="$(stat_number delete_count)"
    if blkdiscard -o "$((first_key * BLOCK_SIZE + 512))" -l 512 \
        "${DEVICE}" 2>"${TMPDIR}/partial-discard.err"; then
        # The block layer may consume a range smaller than the advertised
        # discard granularity as a successful no-op instead of submitting an
        # unaligned discard bio to the target. Both outcomes are safe as long
        # as IMR-LSM publishes no tombstone and preserves the covered blocks.
        discard_result="accepted as a block-layer no-op"
    else
        discard_result="rejected"
    fi
    assert_counter_delta delete_count "${before_delete}" 0 \
        "partial discard publishes no tombstone"
    assert_pair_equals "${first_key}" "${expected_pair}" \
        "partial discard preserves both blocks"
    log "PASS: partial-block discard was ${discard_result}"
}

assert_mkfs_front_device_primitives()
{
    local expected_pair="$1"
    local before_insert
    local output="${TMPDIR}/front-read-1024.bin"

    dd if="${expected_pair}" of="${TMPDIR}/front-first-expected.bin" \
        bs="${BLOCK_SIZE}" count=1 status=none
    dd if="${TMPDIR}/partial-1024.bin" \
        of="${TMPDIR}/front-first-expected.bin" bs=512 seek=2 \
        count=2 conv=notrunc status=none
    dd if="${TMPDIR}/front-first-expected.bin" \
        of="${TMPDIR}/front-pair-expected.bin" bs="${BLOCK_SIZE}" \
        count=1 status=none
    dd if="${expected_pair}" of="${TMPDIR}/front-pair-expected.bin" \
        bs="${BLOCK_SIZE}" skip=1 seek=1 count=1 conv=notrunc status=none

    before_insert="$(stat_number lsm_record_insert_count)"
    dd if="${TMPDIR}/partial-1024.bin" of="${DEVICE}" bs=512 \
        seek=2 count=2 conv=notrunc,fsync status=none
    assert_counter_delta lsm_record_insert_count "${before_insert}" 1 \
        "mkfs-like sector 2 erase publishes one RMW mapping"
    assert_pair_equals 0 "${TMPDIR}/front-pair-expected.bin" \
        "mkfs-like sector 2 erase preserves untouched bytes"

    dd if="${DEVICE}" of="${output}" bs=512 skip=0 count=2 \
        iflag=direct status=none
    dd if="${TMPDIR}/front-first-expected.bin" \
        of="${TMPDIR}/front-read-1024-expected.bin" bs=512 count=2 \
        status=none
    cmp -s "${TMPDIR}/front-read-1024-expected.bin" "${output}" ||
        fail "mkfs-like block 0 read returned unexpected payload"
    log "PASS: mkfs-like block 0 partial read succeeds"

    dd if="${TMPDIR}/partial-1024.bin" \
        of="${TMPDIR}/front-first-expected.bin" bs=512 seek=0 \
        count=2 conv=notrunc status=none
    dd if="${TMPDIR}/front-first-expected.bin" \
        of="${TMPDIR}/front-pair-expected.bin" bs="${BLOCK_SIZE}" \
        count=1 conv=notrunc status=none

    before_insert="$(stat_number lsm_record_insert_count)"
    dd if="${TMPDIR}/partial-1024.bin" of="${DEVICE}" bs=512 \
        seek=0 count=2 conv=notrunc,fsync status=none
    assert_counter_delta lsm_record_insert_count "${before_insert}" 1 \
        "mkfs-like sector 0 erase publishes one RMW mapping"
    assert_pair_equals 0 "${TMPDIR}/front-pair-expected.bin" \
        "mkfs-like sector 0 erase preserves untouched bytes"
}

assert_fresh_front_device_primitives()
{
    local key
    local value
    local before_insert
    local output="${TMPDIR}/fresh-front-read.bin"
    local expected="${TMPDIR}/fresh-front-expected.bin"

    for key in logical_write_count lsm_record_insert_count delete_count \
        compaction_count read_tree_size; do
        value="$(stat_number "${key}")"
        [[ "${value}" -eq 0 ]] ||
            fail "fresh mapper required for empty block 0 coverage: ${key}=${value}"
    done

    dd if=/dev/zero of="${expected}" bs="${BLOCK_SIZE}" count=1 status=none
    read_block 0 "${output}" ||
        fail "fresh unmapped block 0 read failed"
    cmp -s "${expected}" "${output}" ||
        fail "fresh unmapped block 0 did not return zeroes"
    log "PASS: fresh unmapped block 0 reads as zeroes"

    dd if="${TMPDIR}/partial-1024.bin" of="${expected}" bs=512 \
        seek=2 count=2 conv=notrunc status=none
    before_insert="$(stat_number lsm_record_insert_count)"
    dd if="${TMPDIR}/partial-1024.bin" of="${DEVICE}" bs=512 \
        seek=2 count=2 conv=notrunc,fsync status=none
    assert_counter_delta lsm_record_insert_count "${before_insert}" 1 \
        "fresh mkfs-like sector 2 write publishes one RMW mapping"
    assert_block_equals 0 "${expected}" \
        "fresh mkfs-like sector 2 write preserves zero-filled bytes"
}

main()
{
    local zone_count
    local zone_base
    local first_key
    local observer_key
    local zone_boundary_key
    local before_insert

    require_root
    require_tools
    require_debugfs
    imr_lsm_test_safety_begin "${DEVICE}"
    require_test_range
    require_queue_contract

    TMPDIR="$(mktemp -d)"
    trap 'rm -rf -- "${TMPDIR}"' EXIT

    zone_count="$(device_zone_count)"
    zone_base=$((ZONE * BLOCKS_PER_ZONE))
    first_key=$((zone_base + KEY_OFFSET))
    observer_key=$((first_key + 64))
    zone_boundary_key=$(((ZONE + 1) * BLOCKS_PER_ZONE - 1))
    log "device=${DEVICE} zones=${zone_count} first_key=${first_key} observer=${observer_key} boundary=${zone_boundary_key}"

    make_pattern 17 "${BLOCK_SIZE}" "${TMPDIR}/old-first.bin"
    make_pattern 34 "${BLOCK_SIZE}" "${TMPDIR}/old-second.bin"
    make_pattern 51 "${BLOCK_SIZE}" "${TMPDIR}/observer.bin"
    make_pattern 68 "${BLOCK_SIZE}" "${TMPDIR}/new-first.bin"
    make_pattern 85 "${BLOCK_SIZE}" "${TMPDIR}/new-second.bin"
    make_pattern 102 "${BLOCK_SIZE}" "${TMPDIR}/cross-first.bin"
    make_pattern 119 "${BLOCK_SIZE}" "${TMPDIR}/cross-second.bin"
    make_pattern 136 "${BLOCK_SIZE}" "${TMPDIR}/cross-new-first.bin"
    make_pattern 153 "${BLOCK_SIZE}" "${TMPDIR}/cross-new-second.bin"
    make_pattern 170 512 "${TMPDIR}/partial-512.bin"
    make_pattern 187 1024 "${TMPDIR}/partial-1024.bin"

    cat "${TMPDIR}/old-first.bin" "${TMPDIR}/old-second.bin" \
        > "${TMPDIR}/old-pair.bin"
    cat "${TMPDIR}/new-first.bin" "${TMPDIR}/new-second.bin" \
        > "${TMPDIR}/new-pair.bin"
    cat "${TMPDIR}/cross-first.bin" "${TMPDIR}/cross-second.bin" \
        > "${TMPDIR}/cross-pair.bin"
    cat "${TMPDIR}/cross-new-first.bin" "${TMPDIR}/cross-new-second.bin" \
        > "${TMPDIR}/cross-new-pair.bin"

    if [[ "${first_key}" -eq 0 ]]; then
        assert_fresh_front_device_primitives
    fi

    discard_block "${first_key}"
    discard_block "$((first_key + 1))"
    discard_block "${observer_key}"
    write_block "${first_key}" "${TMPDIR}/old-first.bin"
    write_block "${observer_key}" "${TMPDIR}/observer.bin"
    write_block "$((first_key + 1))" "${TMPDIR}/old-second.bin"
    sync

    assert_pair_equals "${first_key}" "${TMPDIR}/old-pair.bin" \
        "8 KiB read follows two independently mapped 4 KiB keys"
    assert_block_equals "${observer_key}" "${TMPDIR}/observer.bin" \
        "8 KiB read does not alias the interleaved observer"

    before_insert="$(stat_number lsm_record_insert_count)"
    write_pair_one_request "${first_key}" "${TMPDIR}/new-pair.bin"
    sync
    assert_counter_delta lsm_record_insert_count "${before_insert}" 2 \
        "one 8 KiB write is split into two mapping records"
    assert_pair_equals "${first_key}" "${TMPDIR}/new-pair.bin" \
        "one 8 KiB write preserves per-block mapping"
    assert_block_equals "${observer_key}" "${TMPDIR}/observer.bin" \
        "one 8 KiB write does not overwrite the observer"

    discard_block "${zone_boundary_key}"
    discard_block "$((zone_boundary_key + 1))"
    write_block "${zone_boundary_key}" "${TMPDIR}/cross-first.bin"
    write_block "$((zone_boundary_key + 1))" "${TMPDIR}/cross-second.bin"
    sync
    assert_pair_equals "${zone_boundary_key}" "${TMPDIR}/cross-pair.bin" \
        "one 8 KiB read splits across logical zones"

    before_insert="$(stat_number lsm_record_insert_count)"
    write_pair_one_request "${zone_boundary_key}" "${TMPDIR}/cross-new-pair.bin"
    sync
    assert_counter_delta lsm_record_insert_count "${before_insert}" 2 \
        "one cross-zone 8 KiB write creates two mapping records"
    assert_pair_equals "${zone_boundary_key}" "${TMPDIR}/cross-new-pair.bin" \
        "one 8 KiB write splits across logical zones"

    assert_partial_data_io_rmw "${first_key}" "${TMPDIR}/new-pair.bin"
    write_pair_one_request "${first_key}" "${TMPDIR}/new-pair.bin"
    sync
    assert_pair_equals "${first_key}" "${TMPDIR}/new-pair.bin" \
        "restore 8 KiB pair before partial discard"
    if [[ "${first_key}" -eq 0 ]]; then
        assert_mkfs_front_device_primitives "${TMPDIR}/new-pair.bin"
        write_pair_one_request "${first_key}" "${TMPDIR}/new-pair.bin"
        sync
        assert_pair_equals "${first_key}" "${TMPDIR}/new-pair.bin" \
            "restore 8 KiB pair after mkfs-like sector primitives"
    fi
    assert_partial_discard_safe "${first_key}" "${TMPDIR}/new-pair.bin"

    imr_lsm_test_wait_level_compaction_idle "${DEBUGFS}"
    log "PASS: multi-block boundary and partial-block safety coverage completed"
}

main "$@"
